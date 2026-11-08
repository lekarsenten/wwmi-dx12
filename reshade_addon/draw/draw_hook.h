#pragma once
// THE DRAW PATH -- every indexed draw the game issues, and the mod geometry put in its place.
#include <cstdint>
#include <reshade.hpp>

extern thread_local uint8_t t_x23_claimed;
extern thread_local uint8_t t_x23_shadow;
extern thread_local uint8_t t_x23_cast;
extern thread_local uint8_t t_x23_hidden;
extern thread_local uint32_t t_x23_injected;
extern thread_local uint8_t t_x23_merges; // SkeletonMerger runs the draw's section entered

// True means we drew the mod in the game's place and its own draw must be suppressed.
bool on_draw_indexed_body(reshade::api::command_list* cmd_list, uint32_t index_count, uint32_t instance_count,
    uint32_t first_index, int32_t vertex_offset, uint32_t first_instance);
