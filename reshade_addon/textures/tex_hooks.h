#pragma once
#include <reshade.hpp>
#include <cstdint>

// The texture module's ReShade hooks (registered in AddonInit) and its resource-lifetime entry.
bool on_update_texture_region(reshade::api::device*, const reshade::api::subresource_data& data,
    reshade::api::resource dest, uint32_t dest_subresource, const reshade::api::subresource_box*);
bool on_copy_buffer_to_texture(reshade::api::command_list*, reshade::api::resource source, uint64_t source_offset,
    uint32_t row_length, uint32_t slice_height, reshade::api::resource dest, uint32_t dest_subresource,
    const reshade::api::subresource_box*);
bool on_copy_texture_region(reshade::api::command_list*, reshade::api::resource source, uint32_t source_subresource,
    const reshade::api::subresource_box*, reshade::api::resource dest, uint32_t dest_subresource,
    const reshade::api::subresource_box*, reshade::api::filter_mode);
bool on_copy_resource(reshade::api::command_list*, reshade::api::resource source, reshade::api::resource dest);
// A resource destroyed: whatever the module held for it goes.
void TexOnDestroyed(uint64_t res);
