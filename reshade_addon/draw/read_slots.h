#pragma once
// The descriptor-table slots a draw's bound shaders read, per root parameter (draw_texswap.cpp).
#include <bitset>
#include <cstdint>
#include <reshade.hpp>
#include "../mods/layout_registry.h"

struct ReadSlots {
    bool known[64];
    uint32_t len[64];
    std::bitset<kTexScanMax> want[64];
};

const ReadSlots& DrawReadSlots(reshade::api::command_list* cmd_list);
