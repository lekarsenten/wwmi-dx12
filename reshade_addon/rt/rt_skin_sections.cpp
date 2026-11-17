// rt/rt_skin_sections.cpp -- PTSKIN: the game's skin dispatches recorded as a mod character's sections.
//
//
// With ray tracing on, the game skins a character in Unreal's GPU skin cache (SkinCacheUpdateBatchCS, one dispatch
// per mesh section) and draws it with a pass-through vertex shader that reads the skinned result: position in slot 0,
// tangents (R16G16B16A16_SNORM x2) in slot 1, color in slot 2, UVs in slot 3, the previous position in slot 4. A mod
// binds its bind-pose buffers in the D3D11 layout instead, so its draw comes out in A-pose with broken shading.
//
// Here, each skin dispatch that writes a mod character's BLAS ring is recorded as a section (where it writes, how
// many vertices, its bone palette). At a mod's pass-through draw of component k, RtPtSkin.cso skins all the mod's
// vertices with section k's palette into our own buffers, and the draw's slots are bound to them in the game's layout.
#include "rt_internal.h"

using namespace rt_portable;

namespace rt_portable {

/** Where a skin-cache shader keeps one section's values in its cb0, in dwords. */
struct SkinCbLayout {
    uint32_t bytes = 0;     // the cbuffer's declared size
    uint32_t out_start = 0; // SkinCacheStart: the section's first vertex in the output buffer
    uint32_t count = 0;     // NumVertices
    uint32_t scale = 0;     // the float the skinned position is multiplied by
};

/** The sections of one mod character: the frame being recorded, and the last complete one. */
struct ModSections {
    uint32_t frame = 0;
    std::vector<SkinSection> collecting; // this frame's, in dispatch order
    std::vector<SkinSection> last;       // the previous frame's, sorted by out_va
    std::vector<SkinSection> older;      // the one before, sorted by out_va: `last`'s previous frame
    std::string signature; // the last complete list's vertex counts, for the change log
    bool logged = false;
};

/** A buffer range a register resolves to. */
struct BoundRange {
    ID3D12Resource* res = nullptr;
    uint64_t va = 0;
    uint64_t bytes = 0;
};

/** The compute registers PTSKIN reads from a skin dispatch, copied out of a root signature's register map. */
struct SkinRegisterPicker {
    RegBind* bones;
    RegBind* out_positions;
    RegBind* params;
    RegBind* weights;

    /**
     * @param map The root signature's register map.
     */
    void operator()(const LayoutRegMap& map) const
    {
        *bones = map.t_cs[0];
        *out_positions = map.u_cs[1];
        *params = map.b_cs[0];
        *weights = map.t_cs[3];
    }
};

std::mutex s_skin_cb_mutex;
std::unordered_map<uint64_t, SkinCbLayout> s_skin_cb; // by compute shader hash

// The mesh a skin dispatch works on, told by where its bone-weight input (t3) starts: that buffer is the mesh's
// own and stays, while the output buffer changes (a new one at every character switch). Learned from dispatches
// already known as a mod's, so that later ones are recognised whatever buffer they write.
std::mutex s_mesh_mutex;
std::unordered_map<uint64_t, int> s_mesh_mod; // the weight input's address -> mod
std::atomic<int> s_mesh_logs{ 0 };

std::mutex s_sections_mutex;
std::map<int, ModSections> s_sections; // by mod

std::mutex s_pso_mutex;
std::unordered_map<uint64_t, bool> s_pass_through; // by graphics PSO: does it read the skin cache's layout?

std::mutex s_once_mutex;
std::set<std::pair<int, int>> s_once; // (what, key) pairs already logged

std::atomic<uint64_t> s_ptskin_draws{ 0 };
std::atomic<uint64_t> s_ptskin_skipped{ 0 };

/**
 * Tells whether a message was not logged yet, and marks it logged. Only in debug.
 *
 * @param what Which message.
 * @param key  What it is about, for example a mod and component.
 * @return true the first time for this pair while debug is on.
 */
bool LogOnce(int what, int key)
{
    if (!g_debug) return false;
    std::lock_guard lock(s_once_mutex);
    return s_once.insert({ what, key }).second;
}

/**
 * Gives the cb0 layout of a skin-cache shader variant from the size its DXIL declares.
 *
 * @param bytes The cbuffer's declared size.
 * @param out   Receives the layout.
 * @return false for a size none of the known variants has.
 */
bool SkinCbLayoutForSize(uint32_t bytes, SkinCbLayout* out)
{
    // The variants found in the game (rt_dump): plain, with morph targets, with cloth.
    if (bytes == 28) {
        *out = { 28, 2, 3, 6 };
        return true;
    }
    if (bytes == 32) {
        *out = { 32, 3, 4, 7 };
        return true;
    }
    if (bytes == 168) {
        *out = { 168, 37, 38, 41 };
        return true;
    }
    return false;
}

/**
 * Remembers where a skin-cache shader keeps its section values, read from the cbuffer size its DXIL declares.
 *
 * @param cs_hash   The shader's hash.
 * @param code      Its bytecode.
 * @param code_size Its size in bytes.
 */
void NoteSkinCbLayout(uint64_t cs_hash, const void* code, size_t code_size)
{
    const std::string_view bytes(static_cast<const char*>(code), code_size);
    if (bytes.find("SkinCacheUpdateBatchCS") == std::string_view::npos) return;
    std::vector<DxilCbuf> cbuffers;
    std::string why;
    if (!DxilCbuffers(code, code_size, cbuffers, &why)) {
        logger::write("PTSKIN cs=%016llx: cbuffers not readable (%s)", (unsigned long long)cs_hash, why.c_str());
        return;
    }
    for (const DxilCbuf& cb : cbuffers) {
        if (cb.space != 0 || cb.reg != 0) continue;
        SkinCbLayout layout;
        if (!SkinCbLayoutForSize(cb.bytes, &layout)) {
            logger::write("PTSKIN cs=%016llx: cb0 is %u bytes, not a known skin-cache layout", (unsigned long long)cs_hash,
                cb.bytes);
            return;
        }
        {
            std::lock_guard lock(s_skin_cb_mutex);
            s_skin_cb[cs_hash] = layout;
        }
        logger::write("PTSKIN cs=%016llx: skin-cache shader, cb0 %u bytes (first vertex out at dword %u, count %u, "
                      "scale %u)",
            (unsigned long long)cs_hash, layout.bytes, layout.out_start, layout.count, layout.scale);
        return;
    }
}

/**
 * Gives the size of one element of a typed buffer view.
 *
 * @param format The view's format.
 * @return Its size in bytes, or 0 for a format not listed here.
 */
uint32_t BufferElementBytes(reshade::api::format format)
{
    switch (format) {
    case reshade::api::format::r32g32b32a32_float:
    case reshade::api::format::r32g32b32a32_uint:
    case reshade::api::format::r32g32b32a32_sint:
        return 16;
    case reshade::api::format::r32g32b32_float:
    case reshade::api::format::r32g32b32_uint:
        return 12;
    case reshade::api::format::r32g32_float:
    case reshade::api::format::r32g32_uint:
    case reshade::api::format::r16g16b16a16_float:
    case reshade::api::format::r16g16b16a16_snorm:
    case reshade::api::format::r16g16b16a16_unorm:
    case reshade::api::format::r16g16b16a16_uint:
        return 8;
    case reshade::api::format::r32_float:
    case reshade::api::format::r32_uint:
    case reshade::api::format::r32_sint:
    case reshade::api::format::r32_typeless:
    case reshade::api::format::r8g8b8a8_snorm:
    case reshade::api::format::r8g8b8a8_unorm:
    case reshade::api::format::r8g8b8a8_uint:
    case reshade::api::format::r16g16_float:
    case reshade::api::format::r16g16_snorm:
        return 4;
    default:
        return 0;
    }
}

/**
 * Finds the buffer range a compute register of the current dispatch is bound to.
 *
 * @param cmd_list The command list the dispatch is recorded on.
 * @param state    The list's recorded bindings.
 * @param bind     Where the register sits in the root signature.
 * @param is_cbv   true for a b register.
 * @param out      Receives the range; a table view's own offset and size are applied.
 * @return false when nothing known is bound there.
 */
bool ResolveComputeRegister(reshade::api::command_list* cmd_list, const CmdState& state, const RegBind& bind,
    bool is_cbv, BoundRange* out)
{
    if (bind.param == 0xFF || bind.param >= 64) return false;
    if (bind.is_table) {
        reshade::api::device* const device = cmd_list->get_device();
        const TableStart start = TableStartOf(device, state.root[bind.param].compute_table());
        ResInfo info;
        if (!start || !g_descriptors.Find({ start.heap, start.base + bind.offset }, info) || !info.is_buffer)
            return false;
        out->res = info.res;
        out->va = info.cached_va;
        out->bytes = info.size;
        if (!info.view_handle) return out->va != 0;
        const reshade::api::resource_view_desc desc = device->get_resource_view_desc({ info.view_handle });
        if (desc.type != reshade::api::resource_view_type::buffer) return out->va != 0;
        if (desc.format == reshade::api::format::unknown) { // structured: offset in bytes already
            out->va += desc.buffer.offset;
            out->bytes = (uint64_t)desc.buffer.structured.count * desc.buffer.structured.stride;
            return out->va != 0;
        }
        const uint32_t element = BufferElementBytes(desc.format);
        if (element == 0) return false; // the view's offset cannot be turned into bytes
        out->va += desc.buffer.offset * element; // ReShade gives a typed view's offset and size in elements
        if (desc.buffer.size != UINT64_MAX) out->bytes = desc.buffer.size * element;
        return out->va != 0;
    }
    if (is_cbv) {
        const RootCbvBind& cbv = g_command_lists.Cbv(cmd_list).binds[bind.param];
        out->res = cbv.res;
        out->va = cbv.va;
        out->bytes = cbv.size;
        return out->va != 0;
    }
    out->res = state.root[bind.param].res;
    out->va = state.root[bind.param].va;
    out->bytes = g_resources.SizeAtVa(out->va);
    return out->va != 0;
}

/**
 * Orders sections by where they write, which is the mesh's section order.
 *
 * @param a A section.
 * @param b Another one.
 * @return true when a comes first.
 */
bool SectionBefore(const SkinSection& a, const SkinSection& b)
{
    return a.out_va < b.out_va;
}

// A character that comes back on screen is skinned into a buffer the module has not seen yet, and only the mod's
// draw, later in the frame, tells whose buffer it is. So that such a frame is drawn too, the module remembers
// which mods drew a section of a given size (s_section_mods), and keeps the sections of that size that no mod
// owns yet (s_unowned) for the draw to find by its vb0 window. Both are guarded by s_sections_mutex.
constexpr size_t kSectionModsCap = 4096;
constexpr size_t kUnownedCap = 512;
constexpr uint32_t kBorrowedSectionFrames = 2; // a section not recorded for the drawing mod is used this long
std::unordered_map<uint64_t, std::vector<int>> s_section_mods; // PaletteKey -> the mods that drew such a section
std::vector<SkinSection> s_unowned;

/**
 * Remembers that a mod has a section of this size. Call with s_sections_mutex held.
 *
 * @param key The section's PaletteKey.
 * @param mod The mod.
 */
void LearnSectionMod(uint64_t key, int mod)
{
    const auto found = s_section_mods.find(key);
    if (found == s_section_mods.end()) {
        if (s_section_mods.size() < kSectionModsCap) s_section_mods[key].push_back(mod);
        return;
    }
    for (int known : found->second) {
        if (known == mod) return;
    }
    found->second.push_back(mod);
}

/**
 * Keeps a section that no mod owns yet, for the draws of this frame and the next ones. Call with s_sections_mutex
 * held.
 *
 * @param section The section.
 */
void NoteUnownedSection(const SkinSection& section)
{
    size_t kept = 0;
    for (size_t i = 0; i < s_unowned.size(); ++i) {
        const bool recent = s_unowned[i].frame + kBorrowedSectionFrames >= g_frame_id;
        if (recent && s_unowned[i].out_va != section.out_va) s_unowned[kept++] = s_unowned[i];
    }
    s_unowned.resize(kept);
    if (s_unowned.size() < kUnownedCap) s_unowned.push_back(section);
    static std::atomic<int> s_unowned_logs{ 0 };
    if (g_debug && s_unowned_logs.fetch_add(1) < 24)
        logger::write("PTSKIN-EARLY frame=%u: a section of a known size (%u vertices) into %llx, a buffer no mod owns "
                      "yet -- kept for the draw",
            g_frame_id, section.count, (unsigned long long)section.out_va);
}

/**
 * Records a skin-cache dispatch that writes a mod character's ring as one of its sections.
 *
 * @param cmd_list The command list the dispatch is recorded on.
 * @param state    The list's recorded bindings.
 * @param cs_hash  The dispatch's compute shader.
 */
void RecordSkinSection(reshade::api::command_list* cmd_list, const CmdState& state, uint64_t cs_hash)
{
    SkinCbLayout cb_layout;
    {
        std::lock_guard lock(s_skin_cb_mutex);
        const auto found = s_skin_cb.find(cs_hash);
        if (found == s_skin_cb.end()) return;
        cb_layout = found->second;
    }
    uint64_t layout = g_command_lists.CmpRoot(cmd_list).layout;
    if (!layout) layout = g_command_lists.CmpLayout(cmd_list);
    RegBind bones;
    RegBind out_positions;
    RegBind params;
    RegBind weights;
    if (!layout || !g_layouts.WithRegMap(layout, SkinRegisterPicker{ &bones, &out_positions, &params, &weights })) {
        if (LogOnce(1, 0))
            logger::write("PTSKIN cs=%016llx: root signature %llx not known", (unsigned long long)cs_hash,
                (unsigned long long)layout);
        return;
    }
    BoundRange out_range;
    BoundRange bones_range;
    BoundRange params_range;
    const bool have_out = ResolveComputeRegister(cmd_list, state, out_positions, false, &out_range);
    const bool have_bones = ResolveComputeRegister(cmd_list, state, bones, false, &bones_range);
    const bool have_params = ResolveComputeRegister(cmd_list, state, params, true, &params_range);
    if (!have_out || !have_bones || !have_params || !params_range.res) {
        if (LogOnce(2, 0))
            logger::write("PTSKIN cs=%016llx: registers not resolved: u1 %s (p%u%s+%u), t0 %s (p%u%s+%u), b0 %s (p%u%s)",
                (unsigned long long)cs_hash, have_out ? "ok" : "MISSING", out_positions.param,
                out_positions.is_table ? " table" : "", out_positions.offset, have_bones ? "ok" : "MISSING",
                bones.param, bones.is_table ? " table" : "", bones.offset, have_params ? "ok" : "MISSING", params.param,
                params.is_table ? " table" : "");
        return;
    }
    uint32_t cb[48] = {};
    const uint32_t got = g_game_cbs.Snapshot(params_range.res, params_range.va, reinterpret_cast<uint8_t*>(cb),
        cb_layout.bytes, true);
    if (got < cb_layout.bytes) {
        if (LogOnce(3, 0))
            logger::write("PTSKIN cs=%016llx: cb0 at %llx not readable (%u of %u bytes)", (unsigned long long)cs_hash,
                (unsigned long long)params_range.va, got, cb_layout.bytes);
        return;
    }
    SkinSection section;
    section.count = cb[cb_layout.count];
    section.out_va = out_range.va + (uint64_t)cb[cb_layout.out_start] * kModPositionStride;
    memcpy(&section.scale, &cb[cb_layout.scale], sizeof(float));
    section.bones_va = bones_range.va;
    section.bones_bytes = bones_range.bytes;
    section.bones_res = bones_range.res;
    section.frame = g_frame_id;
    if (section.count == 0) return;
    int overlap = -1;
    const int exact = g_rt_rings.ModOf(section.out_va, (uint64_t)section.count * kModPositionStride, &overlap);
    int mod = exact >= 0 ? exact : overlap;
    BoundRange weights_range;
    if (ResolveComputeRegister(cmd_list, state, weights, false, &weights_range) && weights_range.va) {
        std::lock_guard lock(s_mesh_mutex);
        if (mod >= 0) {
            s_mesh_mod[weights_range.va] = mod;
        } else {
            const auto mesh_found = s_mesh_mod.find(weights_range.va);
            if (mesh_found != s_mesh_mod.end()) {
                mod = mesh_found->second;
                if (g_debug && s_mesh_logs.fetch_add(1) < 24)
                    logger::write("PTSKIN-MESH mod=%d frame=%u: a dispatch into %llx, a buffer not known as the mod's "
                                  "yet, recognised by its weight input at %llx",
                        mod, g_frame_id, (unsigned long long)section.out_va, (unsigned long long)weights_range.va);
            }
        }
    }
    // Every mod that has a section of this size gets a copy of the palette, also when the output buffer is not
    // known as that mod's yet (or is still taken for another mod's): its draw may be the one that reads it.
    std::vector<int> palette_mods;
    {
        std::lock_guard lock(s_sections_mutex);
        if (mod >= 0) LearnSectionMod(PaletteKey(section), mod);
        const auto known = s_section_mods.find(PaletteKey(section));
        if (known != s_section_mods.end()) palette_mods = known->second;
    }
    if (palette_mods.empty()) return;
    for (int palette_mod : palette_mods) CopySectionPalette(cmd_list, state, palette_mod, section);

    std::lock_guard lock(s_sections_mutex);
    if (mod < 0) {
        NoteUnownedSection(section);
        return;
    }
    ModSections& sections = s_sections[mod];
    if (sections.frame != g_frame_id) {
        if (!sections.collecting.empty()) {
            sections.older.swap(sections.last);
            sections.last.swap(sections.collecting);
            std::sort(sections.last.begin(), sections.last.end(), SectionBefore);
            std::string signature;
            for (const SkinSection& s : sections.last) {
                signature += std::to_string(s.count) + "/" + std::to_string(s.bones_bytes / kBoneBytes) + " ";
            }
            static std::atomic<int> s_signature_logs{ 0 };
            if (signature != sections.signature) {
                if (g_debug && s_signature_logs.fetch_add(1) < 60)
                    logger::write("PTSKIN-LOD mod=%d frame=%u: %zu sections (vertices/bones): %s", mod,
                        sections.frame, sections.last.size(), signature.c_str());
                sections.signature = signature;
            }
            if (!sections.logged && g_debug) {
                sections.logged = true;
                std::string text;
                for (const SkinSection& s : sections.last) {
                    char part[112];
                    snprintf(part, sizeof(part), " [out %llx, %u vertices, %llu bones, scale %.3f]",
                        (unsigned long long)s.out_va, s.count, (unsigned long long)(s.bones_bytes / kBoneBytes),
                        s.scale);
                    text += part;
                }
                logger::write("PTSKIN-SECTIONS mod=%d frame=%u: %zu sections:%s", mod, sections.frame,
                    sections.last.size(), text.c_str());
            }
        }
        sections.collecting.clear();
        sections.frame = g_frame_id;
    }
    for (SkinSection& known : sections.collecting) {
        if (known.out_va == section.out_va) {
            known = section;
            return;
        }
    }
    sections.collecting.push_back(section);
}

/**
 * Adds a section to the ones found in a draw's vb0 window; of two at the same address the newer one stays.
 *
 * @param inside     The sections found so far.
 * @param section    The section to add.
 * @param any_age    true for the drawing mod's own sections; others count only while they are fresh.
 * @param window_va  The start of the window.
 * @param window_end Its end.
 */
void KeepNewestInWindow(std::vector<SkinSection>* inside, const SkinSection& section, bool any_age, uint64_t window_va,
    uint64_t window_end)
{
    if (section.out_va < window_va || section.out_va >= window_end) return;
    if (!any_age && section.frame + kBorrowedSectionFrames < g_frame_id) return;
    for (SkinSection& other : *inside) {
        if (other.out_va == section.out_va) {
            if (section.frame > other.frame) other = section;
            return;
        }
    }
    inside->push_back(section);
}

/**
 * Finds a component's section among those the game skinned into the window its own draw binds at vb0. That window
 * is this mesh's output for this frame, so other meshes' sections and the other of the game's two rings fall
 * outside it. A mesh's sections follow one another without a gap, so the comp-th is found by walking from the
 * window's start, each section beginning where the one before ends. Counting the sections seen instead goes wrong
 * while only some of a frame's dispatches are recorded, or when one from another mesh is left over in the window
 * (a character switch): the component then gets another section's bones, and its vertices fly off.
 *
 * @param mod        The mod.
 * @param comp       The component's index.
 * @param window_va  The start of the game draw's vb0.
 * @param window_end Its end.
 * @param out        Receives the section.
 * @param total      Receives how many sections lie in the window.
 * @return false when the window holds no comp-th section.
 */
bool SectionInWindow(int mod, int comp, uint64_t window_va, uint64_t window_end, SkinSection* out, size_t* total)
{
    std::lock_guard lock(s_sections_mutex);
    *total = 0;
    // The window tells whose the sections are, so those recorded for no mod or for another one count as well
    // while they are fresh: right after a character switch the buffer is not known as this mod's yet.
    std::vector<SkinSection> inside;
    for (const auto& entry : s_sections) {
        const bool own = entry.first == mod;
        const ModSections& sections = entry.second;
        const std::vector<SkinSection>* lists[3] = { &sections.collecting, &sections.last, &sections.older };
        for (const std::vector<SkinSection>* list : lists) {
            for (const SkinSection& section : *list) {
                KeepNewestInWindow(&inside, section, own, window_va, window_end);
            }
        }
    }
    for (const SkinSection& section : s_unowned) {
        KeepNewestInWindow(&inside, section, false, window_va, window_end);
    }
    *total = inside.size();
    if (comp < 0) return false;
    uint64_t cursor = window_va;
    for (int index = 0; index <= comp; ++index) {
        const SkinSection* next = nullptr;
        for (const SkinSection& section : inside) {
            if (section.out_va == cursor) next = &section;
        }
        if (!next) return false;
        if (index == comp) *out = *next;
        cursor += (uint64_t)next->count * kModPositionStride;
    }
    LearnSectionMod(PaletteKey(*out), mod);
    return true;
}

/**
 * Tells whether a graphics PSO reads the skin cache's vertex layout (the ray-tracing pass-through draw).
 *
 * @param pso The PSO (a ReShade pipeline handle).
 * @return true for position at s0, tangents at s1 (+8 = TangentZ, R16G16B16A16_SNORM), previous position at s4.
 */
bool IsPassThroughPso(uint64_t pso)
{
    if (!pso) return false;
    {
        std::lock_guard lock(s_pso_mutex);
        const auto found = s_pass_through.find(pso);
        if (found != s_pass_through.end()) return found->second;
    }
    char text[1024] = {};
    MigotoPort::DescribeInputLayout(pso, text, sizeof(text));
    const bool pass_through = strstr(text, "ATTRIBUTE0@s0+0:f6") && strstr(text, "ATTRIBUTE2@s1+8:f13") &&
                              strstr(text, "ATTRIBUTE14@s4+0:f6");
    std::lock_guard lock(s_pso_mutex);
    s_pass_through[pso] = pass_through;
    return pass_through;
}

void ForgetPassThroughPso(uint64_t pso)
{
    std::lock_guard lock(s_pso_mutex);
    s_pass_through.erase(pso);
}

void SectionsOnDestroyDevice()
{
    {
        std::lock_guard lock(s_sections_mutex);
        s_sections.clear();
        s_unowned.clear();
        s_section_mods.clear();
    }
    {
        std::lock_guard lock(s_mesh_mutex);
        s_mesh_mod.clear();
    }
    {
        std::lock_guard lock(s_pso_mutex);
        s_pass_through.clear();
    }
}

} // namespace rt_portable
