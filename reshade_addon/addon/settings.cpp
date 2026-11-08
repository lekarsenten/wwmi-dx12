#include "settings.h"
#include <windows.h>
#include "../textures/tex_toggles.h"
#include "../instrumentation/diag_gate.h"

AddonSettings g_settings;

static const wchar_t kSection[] = L"wwmi-dx12";

void AddonSettings::Load(const std::wstring& path)
{
    ini_path = path;
    dred = Int(L"dred", 0) != 0;
    debug = Int(L"debug", 0) != 0;
    log_level = Int(L"log_level", g_log_verbosity);
    dx12_debug_layer = Int(L"dx12_debug_layer", 0) != 0;
    decensor = Int(L"decensor", 0) != 0;
    hide_uid = Int(L"hide_uid", 0) != 0;
#ifdef WWMI_DEBUG
    hide_outline = Int(L"hide_outline", 0) != 0;
#endif
    tex_deferred = Int(L"tex_background", 1) != 0;
    texlod_resemblance = Int(L"lod_resemblance", 1) != 0;
    texlod_cache = Int(L"lod_cache", 1) != 0;
    const int budget_step = Int(L"tex_mod_budget", 1);
    tex_mod_budget = budget_step >= 0 && budget_step < kTexBudgetSteps ? budget_step : 1;
    tex_distance = Int(L"tex_distance", 0) != 0;
#ifdef WWMI_DEBUG
    tex_watch_ms = (unsigned)Int(L"tex_watch_ms", 2000);
    tex_vram_cap_mb = (unsigned)Int(L"tex_vram_cap_mb", 0);
#endif
}

int AddonSettings::Int(const wchar_t* key, int fallback) const
{
    return (int)GetPrivateProfileIntW(kSection, key, fallback, ini_path.c_str());
}

std::wstring AddonSettings::String(const wchar_t* key) const
{
    wchar_t raw[MAX_PATH] = {};
    GetPrivateProfileStringW(kSection, key, L"", raw, MAX_PATH, ini_path.c_str());
    return raw;
}

void AddonSettings::Save(const wchar_t* key, bool value) const
{
    if (ini_path.empty()) return;
    WritePrivateProfileStringW(kSection, key, value ? L"1" : L"0", ini_path.c_str());
}

void AddonSettings::Save(const wchar_t* key, int value) const
{
    if (ini_path.empty()) return;
    WritePrivateProfileStringW(kSection, key, std::to_wstring(value).c_str(), ini_path.c_str());
}

void AddonSettings::Save(const wchar_t* key, unsigned value) const
{
    if (ini_path.empty()) return;
    WritePrivateProfileStringW(kSection, key, std::to_wstring(value).c_str(), ini_path.c_str());
}
