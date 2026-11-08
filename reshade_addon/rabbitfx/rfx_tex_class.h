#pragma once
#include <cstdint>

// What RabbitFX's SetTextures shader makes of each of the game's first three textures, by shape and format, and
// which of the mod's maps it binds for them.
int RfxTexClass(uint32_t fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t arr);
bool RfxSetTexturesDecision(const int cls[3], bool have_d, bool have_n, bool have_l, bool have_m, uint8_t& d_mask,
    uint8_t& n_mask, uint8_t& l_mask, uint8_t& m_mask, char flags_out[8]);
