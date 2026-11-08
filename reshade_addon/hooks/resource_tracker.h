#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <reshade.hpp>
#include <d3d12.h>
#include "descriptor_shadow.h" // ResInfo -- what is known of a buffer at its GPU address
#include "../core/state_table.h"

// The game's buffers by GPU address and back, from creation to destruction, and every resource's current state
// from the game's barriers. A bind-time record is a pointer the game may have freed: this is where it is checked.
class ResourceTracker {
public:
    void Add(uint64_t va, const ResInfo& info, uint64_t res);
    // The buffer `res` at `va` forgotten (a relocation of ours, on its own terms).
    void ForgetAt(uint64_t va, uint64_t res);
    // The buffer `res` forgotten; true when it was known, with where it was and how big.
    bool Forget(uint64_t res, uint64_t* va, uint32_t* size);
    bool Live(uint64_t res) const;
    bool Live(const void* res) const { return Live(reinterpret_cast<uint64_t>(res)); }
    // Liveness and the buffer's GPU address in one lookup; `va` is written only when the buffer is live.
    bool VaOf(uint64_t res, uint64_t* va) const;
    // Liveness and the record at the buffer's GPU address in one lookup.
    bool InfoOf(uint64_t res, ResInfo* out) const;
    bool AtVa(uint64_t va, ResInfo* out) const;
    uint32_t SizeAtVa(uint64_t va) const; // 0 when unknown
    size_t BufferCount() const;
    // f(va, const ResInfo&) for every live buffer, under the lock; true from f stops the walk.
    template <class F> void ForEachBuffer(F f) const
    {
        std::lock_guard lock(mutex_);
        for (const auto& kv : at_va_)
            if (f(kv.first, kv.second)) break;
    }

    // The resource's exact D3D12 state (reshade::api::resource_usage is bit-identical), from the barrier hook;
    // COMMON for a mod texture of ours, MigotoPort::kResStateUnknown when never seen.
    uint32_t StateOf(void* res) const;
    void NoteState(uint64_t res, uint32_t state);
    void ForgetState(uint64_t res);

private:
    mutable std::mutex mutex_; // guards at_va_ and va_of_
    std::unordered_map<uint64_t, ResInfo> at_va_;
    std::unordered_map<uint64_t, uint64_t> va_of_;
    mutable ShardedStateTable state_;
};
extern ResourceTracker g_resources;

void on_barrier(reshade::api::command_list* cmd_list, uint32_t count, const reshade::api::resource* res,
    const reshade::api::resource_usage* old_s, const reshade::api::resource_usage* new_s);
bool on_create_resource(reshade::api::device* dev, reshade::api::resource_desc& desc, reshade::api::subresource_data*,
    reshade::api::resource_usage);
void on_init_resource(reshade::api::device* device, const reshade::api::resource_desc& desc,
    const reshade::api::subresource_data* initial_data, reshade::api::resource_usage initial_state,
    reshade::api::resource res);
void on_destroy_resource(reshade::api::device* device, reshade::api::resource res);
bool on_copy_buffer_region(reshade::api::command_list*, reshade::api::resource source, uint64_t source_offset,
    reshade::api::resource dest, uint64_t dest_offset, uint64_t size);
