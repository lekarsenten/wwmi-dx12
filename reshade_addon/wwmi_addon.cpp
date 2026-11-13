// The addon's entry: registration with ReShade, the D3D12 device's activation (our directories, every event hook)
// and the DLL's unload. The hooks' bodies live in their modules; the ones that need the SEH guard are wrapped here.
#include "addon_common.h"
#include <shlwapi.h>
#include "migoto/dump_preview.h"
#include "mods/character_recognition.h"
#include "mods/component_table.h"
#include "mods/game_cb_reader.h"
#include "mods/layout_registry.h"
#include "hooks/pipeline_registry.h"
#include "hooks/cmd_bind_hooks.h"
#include "hooks/command_list_registry.h"
#include "hooks/rt_block_hooks.h"
#include "hooks/resource_tracker.h"
#include "dispatch/dispatch_parts.h"
#include "present/present_hook.h"
#include "instrumentation/watchdogs.h"
#include "instrumentation/debug_pipe.h"
#include "rabbitfx/rfx_patch.h" // RfxPatchWorker::Stop
#include "textures/tex_hooks.h"
#include "textures/tex_look_worker.h"
#include "textures/tex_look.h"
#include "textures/tex_crawl.h"
#include "textures/tex_defer.h"
#include "textures/tex_gc.h"
#include "textures/tex_replace.h"
#include "migoto/dx12_wrap.h" // BackBufferForget
#include "migoto/port_toggles.h" // g_buffers_in_vram
#include "instrumentation/removal_pin.h"
#include "instrumentation/hot_path.h"
#include "instrumentation/game_draw_record.h"
#include "draw/draw_hook.h"
#include "draw/draw_sectex.h"
#include "present/present_log.h"
#include "addon/addon.h"
#include "addon/debug_layer.h"
#include "addon/settings.h"
#include "build_stamp.h"
#include "instrumentation/crash_handler.h"
#include "textures/tex_lod.h"
#include "textures/tex_toggles.h"
#include "ui/overlay.h"
#include "mods/wwmi_runtime.h"
#include "instrumentation/morph_dump.h"
#include "migoto/dx12_bridge.h"
#include "migoto/dx12_pipeline.h"
#include "mods/buffer_relocation.h"
#include "textures/mod_texture_store.h"
#include "mods/mod_set.h"

extern HINSTANCE migoto_handle;

static std::wstring s_win64; // the game exe's folder, with its trailing separator

static void on_destroy_swapchain(swapchain*, bool resize)
{
    if (resize) {
        MigotoPort::BackBufferForget();
        return;
    }
    ::logger::write("Exit: swapchain destroyed -- stopping the watchdogs and saving the persistent settings");
    g_frame_step.ForgetSwapchain();
    g_watchdogs.Stop();
    PortApi::SaveSettingsOnExit();
}

static void on_destroy_device(device*)
{
    g_watchdogs.Stop();
    g_rfx_patch.Stop(); // it creates PSOs on this device
    g_descriptors.UnwatchAll();
    g_tex_look_worker.Stop();
    g_tex_crawl.FlushIfQuiet(0);
    g_tex_lod.OnExit();
    g_tex_look.OnExit();
    g_tex_defer.Stop();
    g_tex_gc.Stop();
    g_tex_replace.Stop();
    PortApi::SaveSettingsOnExit();
}

static device* DeviceOf(device* dev)
{
    return dev;
}
static device* DeviceOf(device_object* obj)
{
    return obj->get_device();
}

#ifdef WWMI_DEBUG
static bool on_draw_guarded(command_list* cmd_list, uint32_t vertex_count)
{
    WWMI_GUARD_BEGIN("on_draw")
    g_tex_draw_watch.NoteNonIndexed(cmd_list, vertex_count);
    return false;
    WWMI_GUARD_END(false)
}

static bool on_draw(command_list* cmd_list, uint32_t vertex_count, uint32_t, uint32_t, uint32_t)
{
    if (g_addon.disabled) return false;
    return on_draw_guarded(cmd_list, vertex_count);
}
#endif

template <auto Fn> struct OwnDevice;
template <typename R, typename First, typename... A, R (*Fn)(First, A...)> struct OwnDevice<Fn> {
    static R call(First first, A... a)
    {
        if (DeviceOf(first) != g_addon.reshade_device) return R();
        return Fn(first, a...);
    }
};
#define OWN_DEVICE(hook) (&OwnDevice<hook>::call)

static void ActivateD3D12();

static void on_init_device(device* dev)
{
    ::logger::write("Init: on_init_device entered (api=%d)", (int)dev->get_api());
    if (dev->get_api() != device_api::d3d12) {
        ::logger::write("Init: not a D3D12 device -- its events are ignored");
        return;
    }
    if (g_addon.reshade_device) {
        ::logger::write("Init: a further D3D12 device -- its events are ignored, the addon stays on the first");
        return;
    }
    g_addon.is_dx12 = true;
    g_addon.reshade_device = dev;
    ActivateD3D12();
    ::logger::write("Init: d3d12 device; calling g_mod_textures.Init");
    g_mod_textures.Init(dev);
    ::logger::write("Init: g_mod_textures.Init returned");

    auto* d3d12_dev = reinterpret_cast<ID3D12Device*>(dev->get_native());
    g_addon.d3d12_device = d3d12_dev;
    if (g_settings.dx12_debug_layer) g_debug_layer.Attach(d3d12_dev);
    ::logger::write("Init: on_init_device complete");
}

extern "C" __declspec(dllexport) const char* NAME = "wwmi-dx12";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Wuthering Waves Mod Importer (DX12)";

// The patched shaders of a build from before cache\shaders, moved there; how many.
static unsigned MoveShaderCache(const std::wstring& from, const std::wstring& to)
{
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW((from + L"\\*.rfx*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.emplace_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    unsigned moved = 0;
    for (const std::wstring& n : names)
        moved += MoveFileExW((from + L"\\" + n).c_str(), (to + L"\\" + n).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    return moved;
}

static bool on_draw_indexed_guarded(command_list* cmd_list, uint32_t index_count, uint32_t instance_count,
    uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
    WWMI_GUARD_BEGIN("on_draw_indexed")
    return on_draw_indexed_body(cmd_list, index_count, instance_count, first_index, vertex_offset, first_instance);
    WWMI_GUARD_END(false) // false = let the game draw its own geometry for this call
}

// Outside the SEH guard on purpose: this layer holds a struct and takes a lock, which the guard function may not
// (C2712). While J1 is not armed this costs one relaxed load and a branch per draw.
static bool on_draw_indexed(command_list* cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t first_index,
    int32_t vertex_offset, uint32_t first_instance)
{
    g_command_lists.NoteDraw(cmd_list);
#ifdef WWMI_DEBUG
    if (g_present_log.left.load(std::memory_order_relaxed) > 0) {
        g_present_log.draws.fetch_add(1, std::memory_order_relaxed);
        g_present_log.frame_end_draws.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    if (g_addon.disabled)
        return on_draw_indexed_guarded(
            cmd_list, index_count, instance_count, first_index, vertex_offset, first_instance);
    ModSetScope set_scope(cmd_list);
    if (!MorphDump::Armed() || !Comps().Loaded())
        return on_draw_indexed_guarded(
            cmd_list, index_count, instance_count, first_index, vertex_offset, first_instance);
    MorphDump::GameDrawRecord r{};
    const bool row = g_game_draw_recorder.Begin(cmd_list, index_count, instance_count, first_index, &r);
    const bool ret =
        on_draw_indexed_guarded(cmd_list, index_count, instance_count, first_index, vertex_offset, first_instance);
    if (row) {
        r.claimed = t_x23_claimed;
        r.shadow = t_x23_shadow;
        r.cast = t_x23_cast;
        r.hidden = t_x23_hidden;
        r.injected = t_x23_injected;
        r.ret = ret ? 1 : 0;
        r.merges = t_x23_merges;
        MorphDump::RecordGameDraw(r);
    }
    return ret;
}

static void on_present(command_queue* queue, swapchain* sc, const rect*, const rect*, uint32_t, const rect*)
{
    WWMI_GUARD_BEGIN("on_present")
    g_frame_step.OnPresent(queue, sc);
    WWMI_GUARD_END_VOID()
}

// Our folders under the game's: the mod tree (wwmi_dir from the ini), the data and cache directories.
static void SetupDirectories()
{
    wchar_t wwmi_dir_buf[MAX_PATH] = {};
    {
        wchar_t raw[MAX_PATH] = {};
        wcscpy_s(raw, g_settings.String(L"wwmi_dir").c_str());
        if (raw[0] && PathIsRelativeW(raw)) {
            const std::wstring joined = s_win64 + raw;
            if (!GetFullPathNameW(joined.c_str(), MAX_PATH, wwmi_dir_buf, nullptr)) wwmi_dir_buf[0] = 0;
        } else {
            wcscpy_s(wwmi_dir_buf, raw);
        }
        const size_t n = wcslen(wwmi_dir_buf);
        if (n && (wwmi_dir_buf[n - 1] == L'\\' || wwmi_dir_buf[n - 1] == L'/')) wwmi_dir_buf[n - 1] = 0;
    }
    if (!wwmi_dir_buf[0]) {
        ::logger::write(
            "Config: wwmi_dx12.ini not found or wwmi_dir not set (looked at %S)", g_settings.ini_path.c_str());
        return;
    }
    MigotoPort::SetDumpDir((std::wstring(wwmi_dir_buf) + L"\\DX12_Dumps").c_str()); // every capture's root
    g_addon.data_dir = s_win64 + L"wwmi_dx12";
    const std::wstring cache_dir = g_addon.data_dir + L"\\cache";
    CreateDirectoryW(g_addon.data_dir.c_str(), nullptr);
    CreateDirectoryW(cache_dir.c_str(), nullptr);
    g_addon.shader_cache_dir = cache_dir + L"\\shaders";
    CreateDirectoryW(g_addon.shader_cache_dir.c_str(), nullptr);
    if (const unsigned moved = MoveShaderCache(cache_dir, g_addon.shader_cache_dir))
        ::logger::write("Config: %u cached shader patch(es) moved into %S", moved, g_addon.shader_cache_dir.c_str());
    if (g_settings.Int(L"clear_cache", 0) != 0) {
        unsigned shaders = 0;
        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileW((g_addon.shader_cache_dir + L"\\*.rfx*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (DeleteFileW((g_addon.shader_cache_dir + L"\\" + fd.cFileName).c_str())) ++shaders;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        const bool lod = DeleteFileW((cache_dir + L"\\lod_tex_cache.lodcache").c_str()) != 0;
        g_settings.Save(L"clear_cache", false);
        ::logger::write("Config: clear_cache=1 -- removed %u cached shader patch(es) and %s texture match cache",
            shaders, lod ? "the" : "no");
    }
    g_addon.wwmi_dir = wwmi_dir_buf;
    g_mod_textures.Reload(); // deferred -- executes on first Tick() after device init
    g_tex_lod.Init(cache_dir);
    g_tex_look.Init(cache_dir);
    ::logger::write("Config: wwmi_dir=%S (their engine reads d3dx.ini there, and every mod it includes)", wwmi_dir_buf);
}

static void RegisterEvents()
{
    reshade::register_event<reshade::addon_event::destroy_device>(OWN_DEVICE(HP(destroy_device)));
    reshade::register_event<reshade::addon_event::destroy_swapchain>(OWN_DEVICE(HP(destroy_swapchain)));
    reshade::register_event<reshade::addon_event::create_resource>(
        OWN_DEVICE(HP(create_resource))); // ALLOW_UNORDERED_ACCESS on a shapekey buffer at creation
    reshade::register_event<reshade::addon_event::init_resource>(OWN_DEVICE(HP(init_resource)));
    reshade::register_event<reshade::addon_event::update_texture_region>(OWN_DEVICE(HP(update_texture_region)));
    reshade::register_event<reshade::addon_event::copy_buffer_to_texture>(OWN_DEVICE(HP(copy_buffer_to_texture)));
    reshade::register_event<reshade::addon_event::copy_texture_region>(OWN_DEVICE(HP(copy_texture_region)));
    reshade::register_event<reshade::addon_event::copy_resource>(OWN_DEVICE(HP(copy_resource)));
    reshade::register_event<reshade::addon_event::copy_buffer_region>(OWN_DEVICE(HP(copy_buffer_region)));
    reshade::register_event<reshade::addon_event::barrier>(OWN_DEVICE(HP(barrier)));
    MigotoPort::SetResourceStateQuery([](void* res) { return g_resources.StateOf(res); });
    {
        const auto cso =
            WwmiReadFile((g_addon.cso_dir + L"\\DumpPreview.cso").c_str()); // the capture's .jpg beside each .dds
        MigotoPort::SetDumpPreviewShader(cso.data(), cso.size());
    }
    /* Same contract as the state query above: the addon owns g_buffer_reloc, the port leaf asks. */
    MigotoPort::g_query_reloc = [](unsigned long long res, unsigned long long* va, unsigned long long* bytes) -> bool {
        RelocBuf rb{};
        if (!g_buffer_reloc.Resolve(reinterpret_cast<ID3D12Resource*>(res), &rb)) return false;
        if (va) *va = rb.va;
        if (bytes) *bytes = rb.bytes;
        return true;
    };
    reshade::register_event<reshade::addon_event::execute_command_list>(
        OWN_DEVICE(HP(execute_command_list))); // submission order vs record order

    reshade::register_event<reshade::addon_event::destroy_resource>(OWN_DEVICE(HP(destroy_resource)));
    reshade::register_event<reshade::addon_event::init_resource_view>(OWN_DEVICE(HP(init_resource_view)));
    reshade::register_event<reshade::addon_event::update_descriptor_tables>(OWN_DEVICE(HP(update_descriptor_tables)));
    reshade::register_event<reshade::addon_event::copy_descriptor_tables>(OWN_DEVICE(HP(copy_descriptor_tables)));
    reshade::register_event<reshade::addon_event::create_pipeline>(
        OWN_DEVICE(HP(create_pipeline))); // shader replacement (DXIL swap by hash)
    reshade::register_event<reshade::addon_event::init_pipeline>(OWN_DEVICE(HP(init_pipeline)));
    reshade::register_event<reshade::addon_event::init_pipeline_layout>(OWN_DEVICE(HP(init_pipeline_layout)));
    reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(OWN_DEVICE(HP(destroy_pipeline_layout)));
    reshade::register_event<reshade::addon_event::destroy_pipeline>(OWN_DEVICE(HP(destroy_pipeline)));
    reshade::register_event<reshade::addon_event::bind_pipeline>(OWN_DEVICE(HP(bind_pipeline)));
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(OWN_DEVICE(HP(bind_descriptor_tables)));
    reshade::register_event<reshade::addon_event::reset_command_list>(
        OWN_DEVICE(HP(reset_command_list))); // the record follows D3D12: nothing survives a Reset
    reshade::register_event<reshade::addon_event::destroy_command_list>(OWN_DEVICE(on_destroy_command_list));
    reshade::register_event<reshade::addon_event::close_command_list>(OWN_DEVICE(HP(close_command_list)));
    reshade::register_event<reshade::addon_event::build_acceleration_structure>(
        OWN_DEVICE(HP(build_acceleration_structure))); // RT ACTIVE + the game's RT work in the pin's tallies
    reshade::register_event<reshade::addon_event::dispatch_rays>(OWN_DEVICE(HP(dispatch_rays)));
    reshade::register_event<reshade::addon_event::push_descriptors>(OWN_DEVICE(HP(push_descriptors)));
    reshade::register_event<reshade::addon_event::push_constants>(
        OWN_DEVICE(HP(push_constants))); // root constants, for putting the game's signature back
    reshade::register_event<reshade::addon_event::bind_vertex_buffers>(OWN_DEVICE(HP(bind_vertex_buffers)));
    reshade::register_event<reshade::addon_event::bind_index_buffer>(OWN_DEVICE(HP(bind_index_buffer)));
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
        OWN_DEVICE(HP(bind_render_targets)));
    // The game's non-PSO state, so IssueDraw can restore what it overwrites (their CommandList.cpp:2385).
    reshade::register_event<reshade::addon_event::bind_viewports>(OWN_DEVICE(HP(bind_viewports)));
    reshade::register_event<reshade::addon_event::bind_scissor_rects>(OWN_DEVICE(HP(bind_scissor_rects)));
    reshade::register_event<reshade::addon_event::bind_pipeline_states>(OWN_DEVICE(HP(bind_pipeline_states)));
    reshade::register_event<reshade::addon_event::draw_indexed>(OWN_DEVICE(HP(draw_indexed)));
#ifdef WWMI_DEBUG
    reshade::register_event<reshade::addon_event::draw>(OWN_DEVICE(&on_draw));
#endif
    reshade::register_event<reshade::addon_event::dispatch>(OWN_DEVICE(HP(dispatch)));
    reshade::register_event<reshade::addon_event::present>(OWN_DEVICE(HP(present)));

    ProfBindLibSteps(); // the lib's timed steps -> their rows, before the overlay can enable the profiler
    DockOverlayPanelOnce();
    reshade::register_event<reshade::addon_event::reshade_open_overlay>(OnOverlayOpen);
    reshade::register_event<reshade::addon_event::reshade_overlay>(DrawOverlayPanel);
}

static void ActivateD3D12()
{
    static std::atomic<bool> s_done{ false };
    if (s_done.exchange(true)) return;
    HotPath::Init();
    CrashHandler::InitReporting(); // report file + module range; needed by the SEH guards
    if (g_addon.crash_handler) CrashHandler::Install(); // the last-chance filter is the optional part
    SetupDirectories();
    RegisterEvents();
    ::logger::write("Init: D3D12 device -- events + overlay registered");
}

static constexpr uint32_t kOldestReShadeApi = 18;

static bool RegisterWithReShade(HMODULE addon_module)
{
    if (reshade::register_addon(addon_module)) {
        g_addon.reshade_api = RESHADE_API_VERSION;
        return true;
    }
    const HMODULE rs = reshade::internal::get_reshade_module_handle();
    const auto reg =
        rs ? reinterpret_cast<bool (*)(void*, uint32_t)>(GetProcAddress(rs, "ReShadeRegisterAddon")) : nullptr;
    const auto imgui = rs ? reinterpret_cast<const imgui_function_table* (*)(uint32_t)>(
                                GetProcAddress(rs, "ReShadeGetImGuiFunctionTable"))
                          : nullptr;
    if (!reg || !imgui) return false;
    for (uint32_t v = RESHADE_API_VERSION - 1; v >= kOldestReShadeApi; --v) {
        if (!reg(addon_module, v)) continue;
        g_addon.reshade_api = v;
        imgui_function_table_instance() = imgui(IMGUI_VERSION_NUM);
        return imgui_function_table_instance() != nullptr;
    }
    return false;
}

// DRED armed before the device exists: 1 on, 0 not wanted, negative = -(the failing HRESULT's low word).
static int ArmDred()
{
    if (!g_settings.dred) return 0;
    ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
    HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&dred));
    if (FAILED(hr) || !dred) return -(int)(hr & 0xffff);
    dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    {
        ID3D12DeviceRemovedExtendedDataSettings1* s1 = nullptr;
        if (SUCCEEDED(dred->QueryInterface(IID_PPV_ARGS(&s1))) && s1) {
            s1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            s1->Release();
        }
    }
    dred->Release();
    return 1;
}

static void EnableDebugLayer()
{
    ID3D12Debug* dbg = nullptr;
    const HRESULT hrd = D3D12GetDebugInterface(IID_PPV_ARGS(&dbg));
    if (SUCCEEDED(hrd) && dbg) {
        dbg->EnableDebugLayer();
        ID3D12Debug1* dbg1 = nullptr;
        if (SUCCEEDED(dbg->QueryInterface(IID_PPV_ARGS(&dbg1))) && dbg1) {
            dbg1->SetEnableSynchronizedCommandQueueValidation(FALSE);
            dbg1->Release();
        }
        dbg->Release();
        ::logger::write("DX12 DEBUG LAYER: ENABLED (ini dx12_debug_layer=1), synchronized queue validation "
                        "off -- still slow; every validation error is logged with DBGLAYER");
    } else {
        ::logger::write("DX12 DEBUG LAYER: requested but unavailable (hr=%08x) -- install the "
                        "Graphics Tools optional feature",
            (unsigned)hrd);
    }
}

// The game's Engine.ini: full-resolution textures and streaming cvars, written only where they differ.
static void ApplyEngineCvars()
{
    std::wstring eng = s_win64 + L"..\\..\\Saved\\Config\\WindowsNoEditor\\Engine.ini";
    const wchar_t* cvars[][2] = { { L"r.Streaming.FullyLoadUsedTextures", L"1" }, { L"r.MipMapLODBias", L"-1" },
        { L"r.Streaming.Boost", L"30" }, { L"r.Streaming.MinBoost", L"30" }, { L"r.Streaming.HLODStrategy", L"2" } };
    unsigned written = 0;
    bool failed = false;
    for (const auto& kv : cvars) {
        wchar_t cur[32] = {};
        GetPrivateProfileStringW(L"SystemSettings", kv[0], L"", cur, (DWORD)std::size(cur), eng.c_str());
        if (wcscmp(cur, kv[1]) == 0) continue; // already set: the file is left untouched
        if (WritePrivateProfileStringW(L"SystemSettings", kv[0], kv[1], eng.c_str()))
            ++written;
        else
            failed = true;
    }
    ::logger::write("Config: Engine.ini full-res + streaming cvars %s (%u written) (%S)",
        failed        ? "FAILED"
            : written ? "applied"
                      : "already set",
        written, eng.c_str());
    g_addon.cfg_engine_path = eng;
    g_addon.cfg_db_path = s_win64 + L"..\\..\\Saved\\LocalStorage\\LocalStorage.db";
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE)
{
    InitModSets();
    if (!RegisterWithReShade(addon_module)) return false;
    migoto_handle = addon_module;
    wchar_t addon_path[MAX_PATH];
    GetModuleFileNameW(addon_module, addon_path, MAX_PATH);
    wchar_t* p = wcsrchr(addon_path, L'\\');
    if (p) *p = L'\0';
    // The ini and the log: next to the game exe (Win64), wherever ReShade loads add-ons from.
    wchar_t win64[MAX_PATH];
    GetModuleFileNameW(nullptr, win64, MAX_PATH);
    p = wcsrchr(win64, L'\\');
    if (p) *(p + 1) = L'\0';
    s_win64 = win64;
    // Our shaders and dxc: wwmi_dx12\ next to the addon, else next to the game exe, else the addon's own dir.
    g_addon.cso_dir = addon_path;
    for (const std::wstring& d : { std::wstring(addon_path) + L"\\wwmi_dx12", s_win64 + L"wwmi_dx12" }) {
        const DWORD a = GetFileAttributesW(d.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            g_addon.cso_dir = d;
            break;
        }
    }
    g_settings.Load(s_win64 + L"wwmi_dx12.ini");
    ::logger::init((s_win64 + L"wwmi_dx12_addon.log").c_str());
    ::logger::set_build_id(WWMI_VERSION " " WWMI_BUILD_SHA); // emitted the moment the level is raised above 0

    const int dred_armed = ArmDred();

    {
        const int lvl = g_settings.log_level;
        SetDiagnostics(g_settings.debug, (lvl < 1) ? 1 : (lvl > 2) ? 2 : lvl);
    }

    // Now that the level is live, say what the pre-log arming actually did.
    if (dred_armed > 0)
        ::logger::write("DRED: breadcrumbs + page-fault tracking FORCED ON");
    else if (dred_armed == 0)
        ::logger::write("DRED: off (dred=1 in the ini turns it on)");
    else
        ::logger::write("DRED: unavailable (hr=....%04x) -- device likely already created; a removal will "
                        "report only a reason code",
            (unsigned)(-dred_armed));
    if (g_addon.reshade_api != RESHADE_API_VERSION)
        ::logger::write("ReShade: add-on API %u accepted, not %u -- a ReShade older than 6.8.0, which is not supported",
            g_addon.reshade_api, (unsigned)RESHADE_API_VERSION);

    MigotoPort::g_buffers_in_vram = g_settings.Int(L"buffers_in_vram", 1) != 0;
    ::logger::write(
        "Config: decensor=%d hide_uid=%d lod_resemblance=%d lod_cache=%d buffers_in_vram=%d tex_mod_budget=%s tex_distance=%d",
        (int)g_settings.decensor, (int)g_settings.hide_uid, (int)g_settings.texlod_resemblance,
        (int)g_settings.texlod_cache, (int)MigotoPort::g_buffers_in_vram, TexBudgetName(g_settings.tex_mod_budget),
        (int)g_settings.tex_distance);
    g_tex_defer.Start();
    if (g_settings.dx12_debug_layer) EnableDebugLayer();
    ::logger::write("Config: debug=%d (diagnostics %s, log level %d)", (int)g_debug, g_debug ? "ON" : "off",
        g_debug ? g_log_verbosity : 0);

    ApplyEngineCvars();

    reshade::register_event<reshade::addon_event::init_device>(on_init_device);
    PortApi::SetParseObserver(&BuildModSet, nullptr);
    PortApi::SetReloadGate(&ReloadGate::MayReload, &g_reload_gate);
    ::logger::write("Init: AddonInit complete; inactive until a D3D12 device is created");
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE)
{
    g_debug_layer.Detach();
    PortApi::SaveSettingsOnExit();
    g_watchdogs.Stop(); // no thread of this DLL may outlive its mapping
    DebugPipe::Shutdown();
    g_rfx_patch.Stop();
    g_descriptors.UnwatchAll(); // a live heap's release must not call into this DLL once it is unmapped
    g_tex_look_worker.Stop();
    g_tex_crawl.FlushIfQuiet(0);
    g_tex_lod.OnExit();
    g_tex_look.OnExit();
    g_tex_defer.Stop();
    g_tex_gc.Stop();
    g_tex_replace.Stop();
    CrashHandler::Uninstall(); // the process's previous filter comes back before this code is unmapped
    reshade::unregister_addon(addon_module);
}
