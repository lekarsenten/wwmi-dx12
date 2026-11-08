#pragma once

// Mods loaded from their engine's parse: the texture overrides, and WWMI's banner stamped with our version.
class ModTexturesLoad {
public:
    void Tick();

private:
    void BannerTick();
    unsigned banner_gen_ = ~0u; // the parse the banner was written for
    bool started_ = false;
};
extern ModTexturesLoad g_mod_textures_load;
