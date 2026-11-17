// rt/rt_portable.cpp -- the portable build's own RT module (WwmiRT=portable), used in place of rt_stub.cpp.
//
// Upstream's RT module is not public, so this one is written from its headers. It records the vertex buffers the
// game builds skinned BLAS from (the skin-cache output "rings") and names the mod character a ring belongs to by its
// index buffer. PTSKIN (rt_skin_*.cpp) then skins a mod's own vertices for the game's ray-tracing draw path. The
// BLAS itself is still built from the game's own mesh, so ray-traced shadows and reflections show the game's model.
//
// This file holds the module's globals, the rings, the BLAS hook, device loss and the overlay; rt_internal.h lists
// the other files.
#include "rt_internal.h"

using namespace rt_portable;

RtDrawVbs g_rt_draw_vbs;
RtSkinRings g_rt_rings;
RtPalRecorder g_rt_pal_recorder;
RtSkinRead g_rt_skin_read;
RtBonesLog g_rt_bones;
RtBlasHook g_rt_blas;
RtDrawPass g_rt_draw;

namespace {

// Rings are looked up on every dispatch, so only mod characters' rebuilt geometry is kept, and only this many.
constexpr size_t kRingCap = 512;
// RTGEO lines: new rings no mod owns (scenery, other characters), and builds whose index buffer is a mod's.
// Each has its own budget, so early scenery cannot use up the lines a mod character needs.
// Counted per build; a build logs one line per input. Small rebuilt meshes (plants, cloth bits) are not logged.
constexpr uint32_t kGeoLogCapOther = 24;
constexpr uint32_t kGeoLogCapMod = 8;
constexpr uint32_t kCharSizedVertices = 2000;
// RTRINGS: a summary of the recorded rings, at most this often.
constexpr uint32_t kSummaryFrames = 600;

std::atomic<uint32_t> s_geo_logged_other{ 0 };
std::atomic<uint32_t> s_geo_logged_mod{ 0 };
std::atomic<uint32_t> s_last_summary_frame{ 0 };
std::atomic<bool> s_cap_reported{ false };
std::atomic<uint64_t> s_builds_seen{ 0 };
std::atomic<uint64_t> s_dynamic_builds{ 0 };

uint64_t NsHash(int mod)
{
    return mod < 0 ? 0 : (uint64_t)mod + 1; // 0 = no mod known
}

} // namespace

// ---- RtDrawVbs: the vb0 a claimed draw of (mod, comp) last read ----

void RtDrawVbs::Note(int mod_id, int comp_idx, const RtDrawVb& v)
{
    const uint64_t key = ((uint64_t)(uint32_t)mod_id << 32) | (uint32_t)comp_idx;
    std::lock_guard lock(mutex_);
    by_comp_[key] = v;
}

bool RtDrawVbs::Of(int mod_id, int comp_idx, RtDrawVb* out) const
{
    const uint64_t key = ((uint64_t)(uint32_t)mod_id << 32) | (uint32_t)comp_idx;
    std::lock_guard lock(mutex_);
    const auto it = by_comp_.find(key);
    if (it == by_comp_.end()) return false;
    if (out) *out = it->second;
    return true;
}

// ---- RtSkinRings: the vertex buffers skinned BLAS are built from ----

void RtSkinRings::Note(uint64_t va, uint64_t bytes, int mod)
{
    if (!va || !bytes) return;
    std::lock_guard lock(mutex_);
    if (mod >= 0) {
        // The game hands a character's old output memory to the next character it shows. A ring another mod
        // owned in this range is no longer that mod's: left in place, it would keep claiming the skin dispatches
        // into it, and this mod would find only some of its sections (drawn in one frame, missing in the next).
        const uint64_t end = va + bytes;
        const uint64_t owner = NsHash(mod);
        std::vector<Ring> kept;
        kept.reserve(rings_.size());
        for (const Ring& ring : rings_) {
            const bool overlaps = va < ring.va + ring.bytes && ring.va < end;
            const bool other_mods = ring.ns_hash != 0 && ring.ns_hash != owner;
            if (!(overlaps && other_mods)) kept.push_back(ring);
        }
        if (kept.size() != rings_.size()) {
            static std::atomic<int> s_taken_logs{ 0 };
            if (g_debug && s_taken_logs.fetch_add(1) < 24)
                logger::write("RTRINGS mod=%d frame=%u: %zu ring(s) of another mod in %llx..%llx forgotten, the range "
                              "is this mod's now",
                    mod, g_frame_id, rings_.size() - kept.size(), (unsigned long long)va, (unsigned long long)end);
            rings_.swap(kept);
        }
    }
    for (Ring& ring : rings_) {
        if (ring.va == va && ring.bytes == bytes) {
            if (mod >= 0) ring.ns_hash = NsHash(mod);
            return;
        }
    }
    if (rings_.size() >= kRingCap) {
        if (g_debug && !s_cap_reported.exchange(true))
            logger::write("RTRINGS: %zu rings recorded, the cap -- later rebuilt BLAS are not watched", kRingCap);
        return;
    }
    rings_.push_back({ va, bytes, NsHash(mod) });
    ++gen_;
}

bool RtSkinRings::Overlaps(uint64_t va, uint64_t bytes) const
{
    if (!va) return false;
    const uint64_t end = va + (bytes ? bytes : 1);
    std::lock_guard lock(mutex_);
    for (const Ring& ring : rings_)
        if (va < ring.va + ring.bytes && ring.va < end) return true;
    return false;
}

int RtSkinRings::ModOf(uint64_t va, uint64_t bytes, int* overlap_mod) const
{
    if (overlap_mod) *overlap_mod = -1;
    const uint64_t end = va + (bytes ? bytes : 1);
    std::lock_guard lock(mutex_);
    for (const Ring& ring : rings_) {
        const int mod = ring.ns_hash ? (int)(ring.ns_hash - 1) : -1;
        if (ring.va == va && ring.bytes == bytes) return mod;
        if (overlap_mod && *overlap_mod < 0 && va < ring.va + ring.bytes && ring.va < end) *overlap_mod = mod;
    }
    return -1;
}

size_t RtSkinRings::Count() const
{
    std::lock_guard lock(mutex_);
    return rings_.size();
}

void RtSkinRings::ForgetRange(uint64_t va, uint64_t bytes)
{
    std::lock_guard lock(mutex_);
    const uint64_t end = va + bytes;
    std::vector<Ring> kept;
    kept.reserve(rings_.size());
    for (const Ring& ring : rings_) {
        const bool inside = ring.va >= va && ring.va + ring.bytes <= end;
        if (!inside) kept.push_back(ring);
    }
    rings_.swap(kept);
}

uint32_t RtSkinRings::Gen() const
{
    std::lock_guard lock(mutex_);
    return gen_;
}

/**
 * Writes one RTRINGS line: how many rings are recorded and how many belong to each mod.
 * Called from the BLAS hook; it writes at most once every kSummaryFrames frames.
 */
static void LogRingSummary()
{
    const uint32_t frame = g_frame_id;
    uint32_t last = s_last_summary_frame.load(std::memory_order_relaxed);
    if (frame - last < kSummaryFrames) return;
    if (!s_last_summary_frame.compare_exchange_strong(last, frame)) return;
    std::map<int, size_t> per_mod;
    size_t total = 0;
    g_rt_rings.CountPerMod(&per_mod, &total);
    std::string text;
    for (const auto& entry : per_mod) {
        char part[48];
        snprintf(part, sizeof(part), " mod%d=%zu", entry.first, entry.second);
        text += part;
    }
    logger::write("RTRINGS frame=%u: %zu rings, owned by a mod:%s", frame, total, text.empty() ? " none" : text.c_str());
}

void RtSkinRings::CountPerMod(std::map<int, size_t>* per_mod, size_t* total) const
{
    std::lock_guard lock(mutex_);
    *total = rings_.size();
    for (const Ring& ring : rings_)
        if (ring.ns_hash) ++(*per_mod)[(int)(ring.ns_hash - 1)];
}

// ---- RtBlasHook: what the game builds its BLAS from ----

uint64_t RtBlasHook::Va(reshade::api::resource r, uint64_t off)
{
    // ReShade passes an address it cannot map to a resource as offset with a null resource.
    if (!r.handle) return off;
    return reinterpret_cast<ID3D12Resource*>(r.handle)->GetGPUVirtualAddress() + off;
}

int RtBlasHook::NameTriangles(const reshade::api::acceleration_structure_build_input& in, uint32_t* first_out)
{
    if (first_out) *first_out = 0;
    if (in.type != reshade::api::acceleration_structure_build_input_type::triangles) return -1;
    const uint64_t index_va = Va(in.triangles.index_buffer, in.triangles.index_offset);
    if (!index_va || !in.triangles.index_count) return -1;
    const uint32_t index_size = in.triangles.index_format == reshade::api::format::r16_uint ? 2u : 4u;
    int mod = -1;
    uint64_t ib_base = 0;
    if (!g_recognition.IbAnchorResolve(index_va, in.triangles.index_count * index_size, &mod, &ib_base)) return -1;
    if (first_out && ib_base <= index_va) *first_out = (uint32_t)((index_va - ib_base) / index_size);
    return mod;
}

bool RtBlasHook::OnBuild(reshade::api::command_list* cmd_list, reshade::api::acceleration_structure_type type,
    reshade::api::acceleration_structure_build_flags build_flags, uint32_t input_count,
    const reshade::api::acceleration_structure_build_input* inputs, reshade::api::resource_view dest,
    reshade::api::acceleration_structure_build_mode mode, unsigned long long n)
{
    s_builds_seen.fetch_add(1, std::memory_order_relaxed);
    if (type == reshade::api::acceleration_structure_type::top_level && inputs && g_debug)
        LogSceneInstances(input_count, inputs);
    if (type != reshade::api::acceleration_structure_type::bottom_level || !inputs) return false;
    // Skinned geometry is built to be updated, and then updated every frame; static scenery is built once.
    const bool dynamic = mode == reshade::api::acceleration_structure_build_mode::update ||
                         (build_flags & reshade::api::acceleration_structure_build_flags::allow_update) != 0;
    if (!dynamic) return false;
    s_dynamic_builds.fetch_add(1, std::memory_order_relaxed);

    // The BLAS belongs to a mod when any of its inputs draws from a mod's index buffer; all inputs then go to it.
    int blas_mod = -1;
    for (uint32_t i = 0; i < input_count && blas_mod < 0; ++i) blas_mod = NameTriangles(inputs[i], nullptr);
    // Only a mod character's vertex buffers are rings: the core learns the skin-cache shader from the first
    // dispatch that writes a ring, so a ring of a plant or a cloth bit would teach it the wrong shader.
    const uint32_t gen_before = g_rt_rings.Gen();
    for (uint32_t i = 0; i < input_count && blas_mod >= 0; ++i) {
        const auto& in = inputs[i];
        if (in.type != reshade::api::acceleration_structure_build_input_type::triangles) continue;
        g_rt_rings.Note(Va(in.triangles.vertex_buffer, in.triangles.vertex_offset),
            (uint64_t)in.triangles.vertex_count * in.triangles.vertex_stride, blas_mod);
    }
    const bool new_ring = blas_mod >= 0 ? g_rt_rings.Gen() != gen_before : true;
    if (blas_mod >= 0 && BuildCutBlas(cmd_list, blas_mod, build_flags, input_count, inputs, dest)) return true;
    // Debug (ptskin_test = 3): a mod character's ray-tracing geometry stops following the animation, to see
    // which parts of a reflection come from it.
    if (blas_mod >= 0 && g_settings.ptskin_test == 3 &&
        mode == reshade::api::acceleration_structure_build_mode::update)
        return true;
    if (!g_debug) return false;
    if (blas_mod >= 0) NoteModBlas(dest.handle, blas_mod, input_count);
    LogRingSummary();

    bool log_it = false;
    if (blas_mod >= 0) {
        log_it = s_geo_logged_mod.fetch_add(1, std::memory_order_relaxed) < kGeoLogCapMod;
    } else if (new_ring) {
        bool char_sized = false;
        for (uint32_t i = 0; i < input_count; ++i)
            if (inputs[i].type == reshade::api::acceleration_structure_build_input_type::triangles &&
                inputs[i].triangles.vertex_count >= kCharSizedVertices)
                char_sized = true;
        if (char_sized) log_it = s_geo_logged_other.fetch_add(1, std::memory_order_relaxed) < kGeoLogCapOther;
    }
    for (uint32_t i = 0; log_it && i < input_count; ++i) {
        const auto& in = inputs[i];
        if (in.type != reshade::api::acceleration_structure_build_input_type::triangles) continue;
        const uint64_t vertex_va = Va(in.triangles.vertex_buffer, in.triangles.vertex_offset);
        uint32_t first = 0;
        const int mod = NameTriangles(in, &first);
        {
            logger::write(
                "RTGEO build #%llu input %u/%u mode=%d flags=%x: vertices va=%llx count=%u stride=%llu fmt=%d | "
                "indices va=%llx count=%u fmt=%d | mod=%d first_index=%u dest=%llx frame=%u%s",
                n + 1, i + 1, input_count, (int)mode, (unsigned)build_flags, (unsigned long long)vertex_va,
                in.triangles.vertex_count, (unsigned long long)in.triangles.vertex_stride,
                (int)in.triangles.vertex_format,
                (unsigned long long)Va(in.triangles.index_buffer, in.triangles.index_offset),
                in.triangles.index_count, (int)in.triangles.index_format, mod < 0 ? blas_mod : mod, first,
                (unsigned long long)dest.handle, g_frame_id, new_ring ? " (new ring)" : "");
        }
    }
    (void)cmd_list;
    return false; // the game's build always runs as it is
}

void RtBlasDiag(reshade::api::command_list*, reshade::api::acceleration_structure_type,
    reshade::api::acceleration_structure_build_flags, uint32_t, const reshade::api::acceleration_structure_build_input*,
    reshade::api::resource_view, reshade::api::acceleration_structure_build_mode, unsigned long long)
{
}

// ---- not written: the skin readback and the bone log ----

bool RtBonesLog::WindowOpen() const
{
    return false;
}
void RtBonesLog::Log(reshade::api::command_list*, const CmdState&, uint32_t, uint64_t, bool) {}
void RtSkinRead::Drain() {}
void RtSkinRead::Record(reshade::api::command_list*, const CmdState&, uint64_t, uint32_t) {}

// ---- device loss ----

void RtOnDestroyDevice()
{
    SkinGpuOnDestroyDevice();
    HideOnDestroyDevice();
    SectionsOnDestroyDevice();
    SceneOnDestroyDevice();
    g_rt_draw.OnDestroyDevice();
}

// ---- overlay and presence ----

void RtDrawOverlay()
{
    ImGui::TextDisabled("  ray tracing (portable RT module)");
    ImGui::TextDisabled("  BLAS builds: %llu   rebuilt (skinned): %llu   rings: %zu",
        (unsigned long long)s_builds_seen.load(), (unsigned long long)s_dynamic_builds.load(), g_rt_rings.Count());
    ImGui::TextDisabled("  PTSKIN draws: %llu   left as they are: %llu", (unsigned long long)s_ptskin_draws.load(),
        (unsigned long long)s_ptskin_skipped.load());
}

bool RtModulePresent()
{
    return true; // the mods stay on while the game ray traces, whatever the ini's rt_experimental says
}
