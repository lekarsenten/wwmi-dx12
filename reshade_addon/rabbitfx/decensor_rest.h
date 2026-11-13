#pragma once
// Decensor rests while the game draws its character menu: at the weapon zoom the game hides the character through
// the fade decensor removes. The menu is known by its backdrop mesh, a small draw (draw_claim.cpp
// RunSmallMeshOverrides).
#include <atomic>
#include <cstdint>
#include "../addon/settings.h"
#include "../core/frame_clock.h"

constexpr uint32_t kMenuBackdropMesh = 0x7017fed2;
constexpr uint32_t kMenuBackdropIndices = 150;

class DecensorRest {
public:
    void MenuDrawn() { menu_frame_.store(g_frame_id, std::memory_order_relaxed); }
    // This frame or the one before: the backdrop's draw can be recorded after the character's.
    bool InMenu() const
    {
        const uint32_t seen = menu_frame_.load(std::memory_order_relaxed);
        return seen && g_frame_id - seen <= 1;
    }

private:
    std::atomic<uint32_t> menu_frame_{ 0 };
};
inline DecensorRest g_decensor_rest;

inline bool DecensorOn()
{
    return g_settings.decensor && !g_decensor_rest.InMenu();
}
