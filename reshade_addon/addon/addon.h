#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <d3d12.h>
#include <reshade.hpp>
#include "../instrumentation/diag_gate.h"

struct Addon {
    std::atomic<bool> disabled{ false }; // every hot handler early-outs: A/B against vanilla
    std::atomic<bool> rt_blocked{ false }; // the game ray traces and this build has no RT module: off until restart
    std::atomic<bool> faulted{ false }; // a guarded handler faulted (crash_handler.cpp): off until restart
    std::wstring cso_dir; // where our compiled shaders live
    std::wstring shader_cache_dir; // patched shaders kept across launches; empty = no cache
    std::wstring data_dir; // <Win64>\wwmi_dx12: our caches and crawls; empty = none
    std::wstring wwmi_dir; // the WWMI folder (d3dx.ini's), set in ActivateD3D12
    std::wstring cfg_engine_path; // the game's Engine.ini and LocalStorage.db, watched by PresentDiag
    std::wstring cfg_db_path;
    bool crash_handler = true; // last-chance reporter -> wwmi_dx12_crash.log
    ID3D12Device* d3d12_device = nullptr;
    reshade::api::device* reshade_device = nullptr; // the same device as ReShade hands it out
    uint32_t reshade_api = 0; // the add-on API ReShade accepted: RESHADE_API_VERSION, or an older one as a fallback
    bool is_dx12 = false; // set once in on_init_device; every hook that assumes a D3D12 device gates on it
    bool capture_mode = false; // Numpad 0 (present_engine.cpp): the capture hotkeys are live only while set
};
extern Addon g_addon;
