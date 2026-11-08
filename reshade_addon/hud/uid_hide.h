// uid_hide.h -- OURS. A5: the player's UID kept off the screen (docs/notes/core/uid_hide.md).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <reshade.hpp>
#include "../rabbitfx/rfx_dxil.h"

class UidHide {
public:
    // The game's UI text pixel shader, told by its interface.
    static bool IsTextShader(const void* code, size_t size);
    // Where the UID is drawn on a back buffer of `target` (width << 16 | height), and in which colours.
    static std::vector<RfxDxilCut> Cuts(uint32_t target);

    void NoteBackBuffer(reshade::api::swapchain* sc);
    uint32_t Target() const { return target_.load(std::memory_order_acquire); } // 0 = not known yet

private:
    std::atomic<uint32_t> target_{ 0 };
};

inline UidHide g_uid_hide;
