#pragma once
#include <string>

struct AddonSettings {
    bool decensor = false;
    bool hide_uid = false; // A5: the UID cut out of the UI text (hud/uid_hide.h)
    bool hide_outline = false;
    bool tex_deferred = true; // mod textures load off the engine lock, vanilla until in (A12)
    // ini tex_hide_loading: with A12 on, a mod that comes on screen is not drawn (nor is the game's model it
    // replaces) until its textures are loaded, instead of showing with the game's textures for a moment. The
    // game's draws of a mesh not yet known as a mod's are left out for its first frames as well.
    bool tex_hide_loading = true;
    bool texlod_resemblance = true; // A13 / ini lod_resemblance: look-alike LOD matches applied (always learned)
    bool texlod_cache =
        true; // A14 / ini lod_cache: LOD matches learned, kept and used; off: none read, learned or written
    int tex_mod_budget = 1; // A19: 0 Original, 1 Auto, 2..5 Auto within 1024/512/256/128 MB per mod
    bool tex_distance = false; // A20: mod textures go down as far as the game draws its own (experimental)
    unsigned tex_watch_ms = 2000; // C12: drawn this long, nothing loading, before a lowered texture comes back
    unsigned tex_vram_cap_mb = 0; // C13: the video memory budget taken as at most this; 0 = Windows'
    bool dx12_debug_layer = false; // takes effect on the next launch
    bool dred = false;
    // ini rt_experimental: keep the mods on while the game ray traces, without an RT module. Ray-traced shadows and
    // reflections then still show the game's own models.
    bool rt_experimental = false;
    // ini ptskin_test (debug): 1 = no motion (the previous frame's positions are this frame's).
    int ptskin_test = 0;
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
