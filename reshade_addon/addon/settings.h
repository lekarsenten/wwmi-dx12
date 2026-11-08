#pragma once
#include <string>

struct AddonSettings {
    bool decensor = false;
    bool hide_uid = false; // A5: the UID cut out of the UI text (hud/uid_hide.h)
    bool hide_outline = false;
    bool tex_deferred = true; // mod textures load off the engine lock, vanilla until in (A12)
    bool texlod_resemblance = true; // A13 / ini lod_resemblance: look-alike LOD matches applied (always learned)
    bool texlod_cache =
        true; // A14 / ini lod_cache: LOD matches learned, kept and used; off: none read, learned or written
    int tex_mod_budget = 1; // A19: 0 Original, 1 Auto, 2..5 Auto within 1024/512/256/128 MB per mod
    bool tex_distance = false; // A20: mod textures go down as far as the game draws its own (experimental)
    unsigned tex_watch_ms = 2000; // C12: drawn this long, nothing loading, before a lowered texture comes back
    unsigned tex_vram_cap_mb = 0; // C13: the video memory budget taken as at most this; 0 = Windows'
    bool dx12_debug_layer = false; // takes effect on the next launch
    bool dred = false;
    bool debug = false;
    int log_level = 0;
    std::wstring ini_path; // wwmi_dx12.ini, read at startup and written back by the overlay

    void Load(const std::wstring& path);
    int Int(const wchar_t* key, int fallback) const;
    std::wstring String(const wchar_t* key) const;
    void Save(const wchar_t* key, bool value) const;
    void Save(const wchar_t* key, int value) const;
    void Save(const wchar_t* key, unsigned value) const;
};
extern AddonSettings g_settings;
