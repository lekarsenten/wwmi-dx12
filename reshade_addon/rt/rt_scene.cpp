// rt/rt_scene.cpp -- RTSCENE (debug): where a mod character sits in the game's ray-tracing scene.
#include "rt_internal.h"

using namespace rt_portable;

namespace rt_portable {

/** A BLAS the game builds from a mod character's mesh. */
struct ModBlas {
    int mod = -1;
    uint32_t geometries = 0;
};

/** The scene instance that uses a mod character's BLAS. */
struct ModInstance {
    int mod = -1;
    uint32_t geometries = 0;
    uint32_t hit_group_index = 0; // InstanceContributionToHitGroupIndex
};

std::mutex s_scene_mutex;
std::unordered_map<uint64_t, ModBlas> s_mod_blas; // by the BLAS's address
ModInstance s_mod_instance;
std::atomic<int> s_scene_logs{ 0 };
std::atomic<int> s_instance_logs{ 0 };
std::atomic<int> s_records_logs{ 0 };

/**
 * Debug: logs the game's scene build, and each instance in it that uses a mod character's BLAS.
 *
 * @param input_count How many inputs the build has.
 * @param inputs      The inputs; the instance lists are read from the game's upload buffers.
 */
void LogSceneInstances(uint32_t input_count, const reshade::api::acceleration_structure_build_input* inputs)
{
    for (uint32_t i = 0; i < input_count; ++i) {
        const auto& in = inputs[i];
        if (in.type != reshade::api::acceleration_structure_build_input_type::instances) continue;
        ID3D12Resource* resource = reinterpret_cast<ID3D12Resource*>(in.instances.buffer.handle);
        const uint64_t va = RtBlasHook::Va(in.instances.buffer, in.instances.offset);
        const uint32_t count = in.instances.count < 16384 ? in.instances.count : 16384;
        std::vector<uint8_t> bytes((size_t)count * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
        const uint32_t read = resource && !in.instances.array_of_pointers && !bytes.empty()
                                  ? g_game_cbs.Snapshot(resource, va, bytes.data(), (uint32_t)bytes.size())
                                  : 0;
        if (s_scene_logs.fetch_add(1) < 6)
            logger::write("RTSCENE build: %u instances at %llx (resource %s, pointers %d): %u bytes readable on the CPU",
                in.instances.count, (unsigned long long)va, resource ? "known" : "unknown",
                (int)in.instances.array_of_pointers, read);
        std::lock_guard lock(s_scene_mutex);
        for (uint32_t k = 0; (k + 1) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC) <= read; ++k) {
            D3D12_RAYTRACING_INSTANCE_DESC desc;
            memcpy(&desc, bytes.data() + (size_t)k * sizeof(desc), sizeof(desc));
            const auto found = s_mod_blas.find(desc.AccelerationStructure);
            if (found == s_mod_blas.end()) continue;
            s_mod_instance.mod = found->second.mod;
            s_mod_instance.geometries = found->second.geometries;
            s_mod_instance.hit_group_index = desc.InstanceContributionToHitGroupIndex;
            if (s_instance_logs.fetch_add(1) < 12)
                logger::write("RTSCENE instance %u of %u: mod=%d blas=%llx (%u geometries) id=%u mask=%02x "
                              "hit_group_index=%u flags=%x frame=%u",
                    k, in.instances.count, found->second.mod, (unsigned long long)desc.AccelerationStructure,
                    found->second.geometries, (unsigned)desc.InstanceID, (unsigned)desc.InstanceMask,
                    (unsigned)desc.InstanceContributionToHitGroupIndex, (unsigned)desc.Flags, g_frame_id);
        }
    }
}

void NoteModBlas(uint64_t blas, int mod, uint32_t input_count)
{
    std::lock_guard lock(s_scene_mutex);
    s_mod_blas[blas] = ModBlas{ mod, input_count };
}

void SceneOnDestroyDevice()
{
    std::lock_guard lock(s_scene_mutex);
    s_mod_blas.clear();
}

} // namespace rt_portable

/**
 * Debug: logs the hit-group records of the last mod instance seen in the scene, as hex words.
 *
 * @param hit_groups The game's hit-group table.
 * @param offset     Where the table starts in it.
 * @param size       The table's size in bytes.
 * @param stride     One record's size in bytes.
 * @param width      The dispatch's width.
 * @param height     The dispatch's height.
 */
void RtNoteDispatchRays(ID3D12Resource* hit_groups, uint64_t offset, uint64_t size, uint64_t stride, uint32_t width,
    uint32_t height)
{
    ModInstance instance;
    {
        std::lock_guard lock(s_scene_mutex);
        instance = s_mod_instance;
    }
    if (instance.mod < 0 || !hit_groups || !stride || stride > 512) return;
    if (s_records_logs.fetch_add(1) >= 4) return;
    const uint64_t table_va = hit_groups->GetGPUVirtualAddress() + offset;
    logger::write("RTSCENE rays %ux%u: hit groups at %llx, %llu bytes, %llu per record; mod=%d starts at record %u",
        width, height, (unsigned long long)table_va, (unsigned long long)size, (unsigned long long)stride, instance.mod,
        instance.hit_group_index);
    // The game may use several records per geometry (one per ray type), so a few more than the geometry count.
    const uint32_t records = instance.geometries * 2 < 24 ? instance.geometries * 2 : 24;
    for (uint32_t r = 0; r < records; ++r) {
        const uint64_t record_offset = (uint64_t)(instance.hit_group_index + r) * stride;
        if (record_offset + stride > size) break;
        uint8_t bytes[512];
        const uint32_t read = g_game_cbs.Snapshot(hit_groups, table_va + record_offset, bytes, (uint32_t)stride);
        std::string words;
        for (uint32_t b = 0; b + 8 <= read; b += 8) {
            uint64_t word = 0;
            memcpy(&word, bytes + b, 8);
            char part[24];
            snprintf(part, sizeof(part), " %llx", (unsigned long long)word);
            words += part;
        }
        logger::write("RTSCENE record +%u (%u bytes read):%s", r, read, words.c_str());
    }
}
