#pragma once
// RT module, BLAS builds: the rings RTPAL finds her skin cache by, and H19 FREEZE.
#include <reshade.hpp>
#include <cstdint>
#include <mutex>
#include <unordered_set>

class RtBlasHook {
public:
    bool OnBuild(reshade::api::command_list* cmd_list, reshade::api::acceleration_structure_type type,
        reshade::api::acceleration_structure_build_flags build_flags, uint32_t input_count,
        const reshade::api::acceleration_structure_build_input* inputs, reshade::api::resource_view dest,
        reshade::api::acceleration_structure_build_mode mode, unsigned long long n);
    static uint64_t Va(reshade::api::resource r, uint64_t off);
    // Which mod component a BLAS triangles input is: its merged-comp slot (ComponentTable), or -1.
    static int NameTriangles(const reshade::api::acceleration_structure_build_input& in, uint32_t* first_out);

private:
    std::mutex freeze_mutex_;
    std::unordered_set<uint64_t> built_; // H19: destinations built once already
};
extern RtBlasHook g_rt_blas;

// RTGEO / RTDBG lines for one build (g_debug).
void RtBlasDiag(reshade::api::command_list* cmd_list, reshade::api::acceleration_structure_type type,
    reshade::api::acceleration_structure_build_flags build_flags, uint32_t input_count,
    const reshade::api::acceleration_structure_build_input* inputs, reshade::api::resource_view dest,
    reshade::api::acceleration_structure_build_mode mode, unsigned long long n);
