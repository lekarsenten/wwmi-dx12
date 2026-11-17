// rt/rt_pal_record.cpp -- RtPalRecorder: which dispatch writes a mod's ring, and what else it binds
// (debug log). Every dispatch that may write a ring is also handed to PTSKIN (rt_skin_sections.cpp).
#include "rt_internal.h"

using namespace rt_portable;

namespace {

// A shader not yet seen writing a mod's ring is examined in one frame out of this many.
constexpr uint32_t kExamEveryFrames = 120;
// RTPAL lines: the first dispatches of each shader that writes a mod's ring, and how many shaders in all.
constexpr uint32_t kPalLogPerCs = 3;
constexpr uint32_t kPalLogTotal = 24;

std::atomic<uint32_t> s_pal_logged{ 0 };

/** One buffer a dispatch binds: a root argument or a descriptor-table slot. */
struct BoundBuffer {
    uint64_t va;
    uint64_t bytes;
    int param;     // root parameter index
    int slot;      // slot in the table, -1 for a root argument
    uint8_t type;  // RootParam::desc_type for a root argument (2 = SRV, 3 = UAV), 0 for a table slot
};

/**
 * Lists the buffers a dispatch binds through its compute root arguments and descriptor tables.
 *
 * @param cmd_list The command list the dispatch is recorded on.
 * @param state    The list's recorded bindings.
 * @param out      Receives the buffers.
 */
void ListBoundBuffers(reshade::api::command_list* cmd_list, const CmdState& state, std::vector<BoundBuffer>* out)
{
    constexpr int kRootCount = 64;
    constexpr int kTableCount = 16;
    constexpr int kSlotCount = 64;
    for (int p = 0; p < kRootCount; ++p) {
        if (!state.root[p].va) continue;
        out->push_back({ state.root[p].va, g_resources.SizeAtVa(state.root[p].va), p, -1, state.root[p].desc_type });
    }
    reshade::api::device* const device = cmd_list->get_device();
    for (int p = 0; p < kTableCount; ++p) {
        const TableStart start = TableStartOf(device, state.root[p].compute_table());
        if (!start) continue;
        for (int i = 0; i < kSlotCount; ++i) {
            ResInfo info;
            if (!g_descriptors.Find({ start.heap, start.base + (uint32_t)i }, info)) continue;
            if (info.cached_va && info.is_buffer) out->push_back({ info.cached_va, info.size, p, i, 0 });
        }
    }
}

/**
 * Describes one shader register of a dispatch: the buffer bound to it, and for a constant buffer its first bytes.
 *
 * @param cmd_list The command list the dispatch is recorded on.
 * @param state    The list's recorded bindings.
 * @param bind     Where the register sits in the root signature.
 * @param name     The register's name for the log, for example "t3".
 * @param is_cbv   true for a b register: its contents are read too.
 * @param out      The text is appended here.
 */
void DescribeRegister(reshade::api::command_list* cmd_list, const CmdState& state, const RegBind& bind,
    const char* name, bool is_cbv, std::string* out)
{
    if (bind.param == 0xFF || bind.param >= 64) return;
    uint64_t va = 0;
    uint64_t bytes = 0;
    ID3D12Resource* res = nullptr;
    if (bind.is_table) {
        const TableStart start = TableStartOf(cmd_list->get_device(), state.root[bind.param].compute_table());
        ResInfo info;
        if (!start || !g_descriptors.Find({ start.heap, start.base + bind.offset }, info)) return;
        va = info.cached_va;
        bytes = info.size;
        res = info.res;
    } else if (is_cbv) {
        const RootCbvBind& cbv = g_command_lists.Cbv(cmd_list).binds[bind.param];
        va = cbv.va;
        bytes = cbv.size;
        res = cbv.res;
    } else {
        va = state.root[bind.param].va;
        bytes = g_resources.SizeAtVa(va);
        res = state.root[bind.param].res;
    }
    if (!va) return;
    char part[160];
    snprintf(part, sizeof(part), " %s=%s%u[%u] %llx/%llu", name, bind.is_table ? "table p" : "root p", bind.param,
        bind.offset, (unsigned long long)va, (unsigned long long)bytes);
    *out += part;
    int ring_overlap = -1;
    if (g_rt_rings.ModOf(va, bytes, &ring_overlap) >= 0 || ring_overlap >= 0) *out += "<RING";
    if (is_cbv && res) {
        uint8_t cb[128];
        const uint32_t got = g_game_cbs.Snapshot(res, va, cb, sizeof(cb), true);
        if (got) {
            *out += "{";
            for (uint32_t d = 0; d + 4 <= got; d += 4) {
                uint32_t value;
                memcpy(&value, cb + d, 4);
                snprintf(part, sizeof(part), "%s%x", d ? "," : "", value);
                *out += part;
            }
            *out += "}";
        }
    }
}

/**
 * Lists a dispatch's bindings by shader register (t#, u#, b#) through its root signature's register map.
 *
 * @param cmd_list The command list the dispatch is recorded on.
 * @param state    The list's recorded bindings.
 * @param out      Receives the text; left empty when the root signature is not known.
 */
/** Writes every compute register of a root signature's register map into the log text. */
struct RegisterDescriber {
    reshade::api::command_list* cmd_list;
    const CmdState* state;
    std::string* text;

    /**
     * @param map The root signature's register map.
     */
    void operator()(const LayoutRegMap& map) const
    {
        char name[8];
        for (uint32_t r = 0; r < sizeof(map.t_cs) / sizeof(map.t_cs[0]) && r < 32; ++r) {
            snprintf(name, sizeof(name), "t%u", r);
            DescribeRegister(cmd_list, *state, map.t_cs[r], name, false, text);
        }
        for (uint32_t r = 0; r < sizeof(map.u_cs) / sizeof(map.u_cs[0]) && r < 16; ++r) {
            snprintf(name, sizeof(name), "u%u", r);
            DescribeRegister(cmd_list, *state, map.u_cs[r], name, false, text);
        }
        for (uint32_t r = 0; r < sizeof(map.b_cs) / sizeof(map.b_cs[0]) && r < 16; ++r) {
            snprintf(name, sizeof(name), "b%u", r);
            DescribeRegister(cmd_list, *state, map.b_cs[r], name, true, text);
        }
    }
};

void DescribeByRegister(reshade::api::command_list* cmd_list, const CmdState& state, std::string* out)
{
    uint64_t layout = g_command_lists.CmpRoot(cmd_list).layout;
    for (int p = 0; p < 64 && !layout; ++p) layout = state.root[p].ctable_note.layout;
    if (!layout) return;
    char head[48];
    snprintf(head, sizeof(head), " layout=%llx:", (unsigned long long)layout);
    std::string text = head;
    if (g_layouts.WithRegMap(layout, RegisterDescriber{ cmd_list, &state, &text })) *out = text;
}

} // namespace

void RtPalRecorder::Record(reshade::api::command_list* cmd_list, const CmdState& state, uint64_t cs_hash)
{
    if (g_rt_rings.Count() == 0) return;
    RecordSkinSection(cmd_list, state, cs_hash);
    if (!g_debug) return;
    const uint32_t ring_gen = g_rt_rings.Gen();
    {
        std::lock_guard lock(mutex_);
        CsNote& note = by_cs_[cs_hash];
        const bool due = note.writes_ring || note.ring_gen != ring_gen ||
                         g_frame_id - note.exam_frame >= kExamEveryFrames || note.exam_frame == 0;
        if (!due) return;
        note.exam_frame = g_frame_id;
        note.ring_gen = ring_gen;
    }

    std::vector<BoundBuffer> bound;
    ListBoundBuffers(cmd_list, state, &bound);
    int ring_mod = -1;
    size_t ring_index = 0;
    for (size_t k = 0; k < bound.size() && ring_mod < 0; ++k) {
        int overlap = -1;
        const int exact = g_rt_rings.ModOf(bound[k].va, bound[k].bytes, &overlap);
        ring_mod = exact >= 0 ? exact : overlap;
        ring_index = k;
    }
    if (ring_mod < 0) return;

    bool first_time = false;
    {
        std::lock_guard lock(mutex_);
        CsNote& note = by_cs_[cs_hash];
        first_time = !note.writes_ring;
        note.writes_ring = true;
    }
    static std::mutex s_count_mutex;
    static std::unordered_map<uint64_t, uint32_t> s_lines_per_cs;
    {
        std::lock_guard lock(s_count_mutex);
        uint32_t& lines = s_lines_per_cs[cs_hash];
        if (lines >= kPalLogPerCs) return;
        ++lines;
    }
    if (s_pal_logged.fetch_add(1, std::memory_order_relaxed) >= kPalLogTotal) return;

    std::string text;
    DescribeByRegister(cmd_list, state, &text);
    for (size_t k = 0; k < bound.size() && text.empty(); ++k) {
        char part[160];
        const BoundBuffer& b = bound[k];
        if (b.slot < 0)
            snprintf(part, sizeof(part), " | root p%d %s va=%llx bytes=%llu", b.param,
                b.type == 3 ? "UAV" : (b.type == 2 ? "SRV" : "CBV"), (unsigned long long)b.va,
                (unsigned long long)b.bytes);
        else
            snprintf(part, sizeof(part), " | table p%d[%d] va=%llx bytes=%llu", b.param, b.slot,
                (unsigned long long)b.va, (unsigned long long)b.bytes);
        text += part;
        if (k == ring_index) text += " <RING";
    }
    logger::write("RTPAL cs=%016llx frame=%u writes mod %d's ring%s:%s", (unsigned long long)cs_hash, g_frame_id,
        ring_mod, first_time ? " (first seen)" : "", text.c_str());
}
