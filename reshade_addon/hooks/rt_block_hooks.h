#pragma once
#include <cstdint>
#include <reshade.hpp>

// The game is ray tracing: with the RT module, its acceleration-structure builds feed the module; without it, the
// add-on turns itself off (RT BLOCK) rather than draw into a lighting pass it cannot follow.
bool on_build_acceleration_structure(reshade::api::command_list* cmd_list,
    reshade::api::acceleration_structure_type type, reshade::api::acceleration_structure_build_flags flags,
    uint32_t input_count, const reshade::api::acceleration_structure_build_input* inputs,
    reshade::api::resource scratch, uint64_t scratch_offset, reshade::api::resource_view source,
    reshade::api::resource_view dest, reshade::api::acceleration_structure_build_mode mode);
bool on_dispatch_rays(reshade::api::command_list* cmd_list, reshade::api::resource raygen, uint64_t raygen_offset,
    uint64_t raygen_size, reshade::api::resource miss, uint64_t miss_offset, uint64_t miss_size, uint64_t miss_stride,
    reshade::api::resource hit_group, uint64_t hit_group_offset, uint64_t hit_group_size, uint64_t hit_group_stride,
    reshade::api::resource callable, uint64_t callable_offset, uint64_t callable_size, uint64_t callable_stride,
    uint32_t width, uint32_t height, uint32_t depth);
