#include "morph_dump.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <ctime>
#include <cstdio>
#include <filesystem>

#include <windows.h>

#include "../core/log.h"
#include "../migoto/dx12_skcap.h" // SkCapArmFrames: J1's buffer contents

namespace MorphDump {
namespace {

struct Row {
    Record r;
    char so[96];
};

std::mutex g_mtx;
std::vector<Row> g_rows;
std::atomic<bool> g_armed{ false };
unsigned g_cap = 0;

// Pass rows live beside the dispatch rows and are flushed by the same Close().
struct PassRow {
    PassRecord r;
    char ns[96];
};
std::vector<PassRow> g_pass;
struct DrainRow {
    DrainRecord r;
    char ns[96];
};
std::vector<DrainRow> g_drain;
struct DrawRow {
    DrawRecord r;
    char ns[96];
}; // one per claimed draw
std::vector<DrawRow> g_draw;
struct GameDrawRow {
    GameDrawRecord r;
    char ns[96];
}; // one per GAME draw of a mod component
std::vector<GameDrawRow> g_gdraw;
std::atomic<bool> g_wait_instance{ false };

std::wstring OutputDir()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::filesystem::path p(buf);
    return p.parent_path().wstring();
}

} // namespace

void Arm(unsigned max_records)
{
    Close(); // restart, never append to a stale capture
    {
        std::lock_guard<std::mutex> l(g_mtx);
        g_cap = max_records ? max_records : 4096;
        g_rows.clear();
        g_rows.reserve(g_cap);
        g_pass.clear();
        g_drain.clear();
        g_draw.clear();
        g_gdraw.clear();
    }
    g_armed.store(true);
    logger::write("MORPHDUMP: armed for %u compute dispatches -- EVERY dispatch is recorded, with no mod, "
                  "on-screen or shader filter, so unmodded characters land in the same table",
        g_cap);
}

bool Armed()
{
    return g_armed.load(std::memory_order_relaxed);
}

void ArmCapture()
{
    Arm(8192); // structure: dispatch / pass / drain tables, ~240 frames
    MigotoPort::SkCapArmFrames(10); // contents:  per-frame ledger + digests + small slots in full
    ProbeArm(240); // the one-shot log probes, same window; re-arms DiagOnce too
    // Not g_diag_gen: that is the SKINPROBE / POOLDRAW capture B3 arms, and bumping it restarts it.
}

void ArmCaptureAtNewInstance()
{
    g_wait_instance.store(true);
    logger::write("MORPHDUMP: waiting -- the capture arms at the next relocation of a mod's buffer, its next new "
                  "instance (each of an ult's copies is one)");
}

bool CaptureWaiting()
{
    return g_wait_instance.load(std::memory_order_relaxed);
}

void NoteNewInstance(uint32_t hash)
{
    if (!g_wait_instance.load(std::memory_order_relaxed) || !g_wait_instance.exchange(false)) return;
    logger::write("MORPHDUMP: %08x relocated for a new instance -- arming the capture", hash);
    ArmCapture();
}

// ---- the erase ring: 8192 handles, lapping. Sized so a character switch's destroys cannot lap it.
static constexpr size_t kErasedRing = 8192;
static std::atomic<uint64_t> g_erased[kErasedRing];
static std::atomic<size_t> g_erased_at{ 0 };

void NoteResourceErased(uint64_t handle)
{
    if (!handle) return;
    const size_t i = g_erased_at.fetch_add(1, std::memory_order_relaxed) % kErasedRing;
    g_erased[i].store(handle, std::memory_order_relaxed);
}

int WasResourceErased(uint64_t handle)
{
    if (!handle) return 0;
    for (size_t i = 0; i < kErasedRing; ++i)
        if (g_erased[i].load(std::memory_order_relaxed) == handle) return 1;
    return 0;
}

void RecordDrain(const DrainRecord& rec)
{
    if (!g_armed.load(std::memory_order_relaxed)) return;
    DrainRow row{};
    row.r = rec;
    if (rec.ns && *rec.ns) _snprintf_s_l(row.ns, sizeof(row.ns), _TRUNCATE, "%ls", logger::utf8_locale(), rec.ns);
    row.r.ns = nullptr;
    std::lock_guard<std::mutex> l(g_mtx);
    if (g_drain.size() < 65536) g_drain.push_back(row);
}

void RecordDraw(const DrawRecord& rec)
{
    if (!g_armed.load(std::memory_order_relaxed)) return;
    DrawRow row{};
    row.r = rec;
    if (rec.ns && *rec.ns) _snprintf_s_l(row.ns, sizeof(row.ns), _TRUNCATE, "%ls", logger::utf8_locale(), rec.ns);
    row.r.ns = nullptr;
    std::lock_guard<std::mutex> l(g_mtx);
    if (g_draw.size() < 65536) g_draw.push_back(row);
}

void RecordGameDraw(const GameDrawRecord& rec)
{
    if (!g_armed.load(std::memory_order_relaxed)) return;
    GameDrawRow row{};
    row.r = rec;
    if (rec.ns && *rec.ns) _snprintf_s_l(row.ns, sizeof(row.ns), _TRUNCATE, "%ls", logger::utf8_locale(), rec.ns);
    row.r.ns = nullptr;
    std::lock_guard<std::mutex> l(g_mtx);
    if (g_gdraw.size() < 65536) g_gdraw.push_back(row);
}

void RecordPass(const PassRecord& rec)
{
    if (!g_armed.load(std::memory_order_relaxed)) return;
    PassRow row{};
    row.r = rec;
    if (rec.ns && *rec.ns) _snprintf_s_l(row.ns, sizeof(row.ns), _TRUNCATE, "%ls", logger::utf8_locale(), rec.ns);
    row.r.ns = nullptr; // the copy in `ns` is the one that survives the flush
    std::lock_guard<std::mutex> l(g_mtx);
    if (g_pass.size() < 65536) g_pass.push_back(row);
}

void RecordDispatch(const Record& rec)
{
    if (!g_armed.load(std::memory_order_relaxed)) return;

    Row row{};
    row.r = rec;
    if (rec.so_section && *rec.so_section)
        _snprintf_s_l(row.so, sizeof(row.so), _TRUNCATE, "%ls", logger::utf8_locale(), rec.so_section);
    row.r.so_section = nullptr; // the copy in `so` is the one that survives

    bool full = false;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        if (g_rows.size() < g_cap) g_rows.push_back(row);
        full = (g_rows.size() >= g_cap);
    }
    if (full) Close();
}

void Close()
{
    if (!g_armed.exchange(false)) return;

    std::vector<Row> rows;
    std::vector<PassRow> passes;
    std::vector<DrainRow> drains;
    std::vector<DrawRow> draws;
    std::vector<GameDrawRow> gdraws;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        rows.swap(g_rows);
        passes.swap(g_pass);
        drains.swap(g_drain);
        draws.swap(g_draw);
        gdraws.swap(g_gdraw);
    }
    if (rows.empty()) {
        logger::write("MORPHDUMP: closed with no rows");
        return;
    }

    wchar_t name[128];
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    swprintf_s(name, L"\\morph_dump-%04d%02d%02d-%02d%02d%02d.tsv", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    const std::wstring path = OutputDir() + name;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        logger::write("MORPHDUMP: could not open the output file -- %zu rows lost", rows.size());
        return;
    }

    fprintf(f,
        "# Every compute dispatch while armed. No mod / on-screen / shader filter.\n"
        "# u0_declared: 1 = a [TextureOverride] declares this buffer (a mod owns it), 0 = NO mod\n"
        "#              declares it (an unmodded character), -1 = u0 could not be resolved.\n"
        "# skip:        1 = we CANCELLED the game's dispatch. On a u0_declared=0 row that is us\n"
        "#              altering an unmodded character's pipeline, which must never happen.\n"
        "# fired:       sections our own dispatch-firing loop ran. Non-zero on a u0_declared=0 row\n"
        "#              is the same violation by the other route.\n"
        "# u0_slot_age: frames since a hook of OURS last wrote that descriptor slot; -1 = never.\n"
        "#              A large age while the character dispatches every frame means the game\n"
        "#              re-pointed the descriptor through a route we do not observe -- the record\n"
        "#              is UNOBSERVED, not merely wrong, and no downstream identity trick fixes it.\n"
        "# u0_slot_src: 0 = never written by a hook we own, 1 = update_descriptor_tables,\n"
        "#              2 = copy_descriptor_tables.\n"
        "# queued:      command lists the observer CAPTURED. `fired` counts sections we invoked and\n"
        "#              says nothing about this -- a declared dispatch with queued=0 runs no pass.\n"
        "# ck_*:        THE CHAIN'S OWN VERDICT for this dispatch, from SkU0Ident. THESE, not the\n"
        "#              u0_* columns, are what the chain acted on: u0_* stop before the size-recover\n"
        "#              step in SkResolveU0, so u0_size=0 with ck_size>0 means the\n"
        "#              recovery WORKED and the dump resolver is simply the shorter path.\n"
        "#              ck_why: 0 ok, 1 no u0 bound, 2 heap MISS, 3 no resource,\n"
        "#                      4 EXTENT-0-RESOURCE-NOT-LIVE (g_res_to_va miss), 5 recovery tried.\n"
        "# ct_match:     WAS THE TABLE WE READ THE ONE THIS DISPATCH BOUND? 1 yes; 0 NO -- it was\n"
        "#               bound under a DIFFERENT compute root signature. D3D12 DISCARDS every root\n"
        "#               table binding on a signature change, and root[].ctable is never invalidated, so\n"
        "#               a 0 means ck_off/heap/slot were derived from a binding the GPU no longer\n"
        "#               has and the resource they name is a coincidence. -1 = param never bound.\n"
        "#               ct_age = frames since that bind (0 = this frame). ct_seqd = binds on this\n"
        "#               list since. READ THIS FIRST: ct_match=0 on the frozen captures and 1 on the\n"
        "#               working ones settles the whole question; equal values REFUTE staleness and\n"
        "#               send the search back to the {heap,slot} content.\n"
        "# ck_off/heap/slot: THE ADDRESS cs-u0 resolved to. Step 5 is a DIRECT ARRAY INDEX, not a\n"
        "#              search, so the same address always returns the same entry. DIFF these between\n"
        "#              a working and a frozen capture: IDENTICAL means we read the same slot and its\n"
        "#              CONTENT changed -- we missed a descriptor write. DIFFERENT means the root\n"
        "#              signature resolve (step 2) moved and the address is what changed.\n"
        "# ck_age/src:  provenance of the entry found there. src 1 = update_descriptor_tables,\n"
        "#              2 = CopyDescriptors, 3 = a view created straight into the slot, 0 = none of\n"
        "#              them, i.e. no hook of ours ever wrote it. Those are the three ways a descriptor\n"
        "#              enters a heap we track, so src=0 (or a very old age while the game dispatches\n"
        "#              every frame) is the missed write that makes the mirror wrong.\n"
        "# ck_buf/tex:  WHAT the slot answered with (ResInfo::is_buffer / tex_cand).\n"
        "#              ck_buf=0 means cs-u0 resolved to a NON-BUFFER -- the {heap,slot} we\n"
        "#              computed is not u0 at all, and a texture can NEVER be in g_res_to_va\n"
        "#              (GetGPUVirtualAddress is 0 for a non-buffer), which is exactly the\n"
        "#              u0_erased=0 + ck_why=4 + size=0 signature. Fix the RESOLVE, not the map.\n"
        "#              ck_buf=1 means it IS a buffer we never saw created -- a different bug.\n"
        "# u0_erased:   ck_why=4 says the resource is absent from g_res_to_va; this says WHY.\n"
        "#              1 = WE erased it in on_destroy_resource (destroy/recreate race, or a\n"
        "#                  recycled pointer) -- the erase is the bug.\n"
        "#              0 = we NEVER had it: on_init_resource never fired for this buffer, or it\n"
        "#                  had no GPU VA at creation. The MISSING RECORD is the bug, and fixing\n"
        "#                  the erase would change nothing.\n"
        "#              res_map_n proves the map is populated, so 'absent' is not 'map empty'.\n"
        "# so_match:    the ShaderOverride matcher's ANSWER, separate from its payload:\n"
        "#              -1 not evaluated, 0 matcher said NO, 1 matched but the section was empty,\n"
        "#              2 matched with a section. shader_override empty with so_match=0 is the\n"
        "#              EXPECTED DX12 state (DX11-hash-keyed entries cannot match DXIL); empty\n"
        "#              with so_match=2 would mean the column, not the matcher, is broken.\n"
        "# u0_why:      WHERE cs-u0 resolution stopped. 10 = resolved. 1 no compute layout, 2 the\n"
        "#              layout declares no cs-u0, 3 root param bound but empty, 4 resource in hand but\n"
        "#              NO g_res_to_va record, 5 VA found but NO g_va_to_res record, 6 va route missed,\n"
        "#              7 no descriptor table on the param, 8 table route resolved nothing.\n"
        "#              4 and 5 are MEMORY-MAP misses: the binding is correct and we lost the size.\n"
        "# q_reason:    0 queued, 1 not-shapekey, 2 unused, 3 no-run-command, 4 duplicate.\n"
        "# geo_lo/geo_hi: the byte range u0 MUST have for this dispatch, from the game's own geometry\n"
        "#              (Loader: the preceding per-dword clear, groups x 256; Multiplier: groups x 32\n"
        "#              vertices x 24 B). 0/0 = nothing known (no clear seen on this list this frame).\n"
        "# geo:         what the resolver did with the slot's record against that range: 0 no range,\n"
        "#              1 the record fits, 2 REPLACED by the fitting live declared buffer, 3 dropped\n"
        "#              (nothing fits, cs-u0 cleared), 4 dropped (more than one fits, cs-u0 cleared).\n");
    fprintf(f,
        "seq\tframe\tcs_hash\tgx\tgy\tgz\tcmd_list\tcmp_layout\t"
        "u0_param\tu0_res\tu0_size\tu0_desc\tu0_declared\towner\tu0_slot_age\tu0_slot_src\tu0_why\t"
        "ct_match\tct_age\tct_seqd\t"
        "ck_why\tck_size\tck_decl\tck_live\tck_push\tck_claims\tck_buf\tck_tex\t"
        "ck_off\tck_heap\tck_slot\tck_age\tck_src\tgeo_lo\tgeo_hi\tgeo\t"
        "u0_erased\tres_map_n\t"
        "b0_param\tcb0_res\tcb0_va\tcb0_size\tskip\tfired\tqueued\tq_reason\t"
        "so_match\tshader_override\n");

    unsigned seq = 0;
    for (const Row& row : rows) {
        const Record& r = row.r;
        fprintf(f,
            "%u\t%u\t%016llx\t%u\t%u\t%u\t%llx\t%llx\t"
            "%d\t%llx\t%u\t%08x\t%d\t%d\t%d\t%u\t%u\t"
            "%d\t%d\t%d\t"
            "%d\t%u\t%d\t%d\t%d\t%d\t%d\t%d\t"
            "%u\t%llx\t%u\t%d\t%d\t%u\t%u\t%d\t"
            "%d\t%u\t"
            "%d\t%llx\t%llx\t%u\t%d\t%u\t%u\t%u\t%d\t%s\n",
            seq++, r.frame, (unsigned long long)r.cs_hash, r.gx, r.gy, r.gz, (unsigned long long)r.cmd_list,
            (unsigned long long)r.cmp_layout, r.u0_param, (unsigned long long)r.u0_res, r.u0_size, r.u0_desc_hash,
            r.u0_declared, r.owner, r.u0_slot_age, (unsigned)r.u0_slot_src, (unsigned)r.u0_why, r.ct_match, r.ct_age,
            r.ct_seqd, r.ck_why, r.ck_size, r.ck_decl, r.ck_live, r.ck_push, r.ck_claims, r.ck_buf, r.ck_tex, r.ck_off,
            (unsigned long long)r.ck_heap, r.ck_slot, r.ck_age, r.ck_src, r.geo_lo, r.geo_hi, r.geo_verdict,
            r.u0_erased, r.res_map_n, r.b0_param, (unsigned long long)r.cb0_res, (unsigned long long)r.cb0_va,
            r.cb0_size, r.skip ? 1 : 0, r.sections_fired, r.queued, (unsigned)r.queue_reason, r.so_match, row.so);
    }
    fclose(f);

    if (!passes.empty()) {
        wchar_t pname[128];
        swprintf_s(pname, L"\\morph_pass-%04d%02d%02d-%02d%02d%02d.tsv", tmv.tm_year + 1900, tmv.tm_mon + 1,
            tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        FILE* pf = nullptr;
        if (_wfopen_s(&pf, (OutputDir() + pname).c_str(), L"wb") == 0 && pf) {
            fprintf(pf,
                "# One row per pass OUR engine ran. Join to morph_dump on (frame, queued_cs).\n"
                "# Acceptance test -- docs/DX11-SHAPEKEY-CHAIN-CONTRACT.md 'What a DX12 capture must match':\n"
                "#   check 2  passes per queued_cs   one-batch mod 2+2  two-batch mod 2+2+1\n"
                "#   check 3  Overrider emitted       1,1,1           1,1,1 twice\n"
                "#   check 4  Loader emitted_y        3674            1644 then 542\n"
                "#   check 5  Multiplier emitted_y    5978            7771  (a FLOOR: > the game's 2698 is CORRECT)\n"
                "#   check 6  batch selector          n/a             queued_tgy 1644 / 542\n"
                "#   check 8  Applier                 t6=1 u6=1       absent\n"
                "# The Overrider's params. One-batch mods: x0=checksum, rest 0. Two-batch mods pack BOTH:\n"
                "#     x0=926 y0=0 z0=0 / x1=825 y1=52582 z1=52582. The ini saves and restores these\n"
                "#     around the pass, so non-zero leftovers on a later pass = the restore failed.\n"
                "# vb0_state set while vb0_shim = 0 means the ini named the resource and the\n"
                "#      substitution never reached the D3D12 list -- the shapekeyed copy was refused.\n"
                "#      Own-Applier mods want vb0_shim != 0 and vb6 null; the rest want u0 at vb6.\n"
                "# emitted_n = 0 means the pass ran but issued NO dispatch.\n"
                "# t33 = 0 is a known standing divergence: the Overrider's offsets buffer.\n"
                "# check 7: cb0_res is the PipeState's b0 and is NOT what the pass runs on. snap_res is.\n"
                "#     DISTINCT snap_res across the Loader and Multiplier passes is the precondition for\n"
                "#     the reciprocal cb0[64]; one snap_res for all four is the kaleidoscope. snap_hit=0\n"
                "#     means no snapshot existed for that cs and the pass ran on whatever was left bound.\n"
                "#     The cb0[64] FLOAT itself still needs SKCAP -- the snapshot is HEAP_TYPE_DEFAULT.\n"
                "# vb_nonnull == 0 means nothing bound a vertex buffer through the shim ANYWHERE in\n"
                "#     the process, so vb0_*/vb6_* zeros are REAL. vb_nonnull > 0 with vb_last_ps !=\n"
                "#     ps_addr means we sample a DIFFERENT PipeState than the shim writes and the zeros\n"
                "#     are an artefact.\n");
            fprintf(pf,
                "seq\tframe\tns\tmod\towner\tpass\tof\tqueued_cs\tqueued_tgy\t"
                "emitted_n\temitted_x\temitted_y\temitted_z\t"
                "t0\tt1\tt6\tt33\tu5\tu6\tchecksum\tshapekey_vtx\tcustom_vtx\t"
                "x0\ty0\tz0\tx1\ty1\tz1\t"
                "cb0_res\tsnap_hit\tsnap_res\tsnap_va\tsnap_bytes\t"
                "ps_addr\tvb_last_ps\tvb_binds\tvb_nonnull\t"
                "vb0_state\tvb0_shim\tvb0_size\tvb0_stride\tvb6_state\tvb6_shim\n");
            unsigned pseq = 0;
            for (const PassRow& pr : passes) {
                const PassRecord& r = pr.r;
                fprintf(pf,
                    "%u\t%u\t%s\t%d\t%d\t%u\t%u\t%016llx\t%u\t"
                    "%u\t%u\t%u\t%u\t"
                    "%d\t%d\t%d\t%d\t%d\t%d\t%g\t%g\t%g\t"
                    "%g\t%g\t%g\t%g\t%g\t%g\t"
                    "%llx\t%d\t%llx\t%llx\t%u\t"
                    "%llx\t%llx\t%u\t%u\t"
                    "%llx\t%llx\t%u\t%u\t%llx\t%llx\n",
                    pseq++, r.frame, pr.ns, r.mod_id, r.owner_mod, r.pass_index, r.pass_count,
                    (unsigned long long)r.queued_cs, r.queued_tgy, r.emitted_n, r.emitted_x, r.emitted_y, r.emitted_z,
                    r.t0, r.t1, r.t6, r.t33, r.u5, r.u6, r.checksum, r.shapekey_vtx, r.custom_vtx, r.x0, r.y0, r.z0,
                    r.x1, r.y1, r.z1, (unsigned long long)r.cb0_res, r.snap_hit, (unsigned long long)r.snap_res,
                    (unsigned long long)r.snap_va, r.snap_bytes, (unsigned long long)r.ps_addr,
                    (unsigned long long)r.vb_last_ps, r.vb_binds, r.vb_nonnull, (unsigned long long)r.vb0_state,
                    (unsigned long long)r.vb0_shim, r.vb0_shim_size, r.vb0_shim_stride, (unsigned long long)r.vb6_state,
                    (unsigned long long)r.vb6_shim);
            }
            fclose(pf);
            logger::write("MORPHDUMP: wrote %zu PASS rows to morph_pass-*.tsv -- join on (frame, queued_cs) "
                          "and read the acceptance test in its header.",
                passes.size());
        } else {
            logger::write("MORPHDUMP: could not open the pass file -- %zu pass rows lost", passes.size());
        }
    } else {
        logger::write("MORPHDUMP: NO PASS ROWS. Either the capture caught no draw-time drain, or the chain "
                      "ran no passes at all -- which is itself the finding. The drain table says which.");
    }

    // ---- THE DRAIN TABLE: every call, every exit --------------------------------------------------
    if (!drains.empty()) {
        wchar_t dname[128];
        swprintf_s(dname, L"\\morph_drain-%04d%02d%02d-%02d%02d%02d.tsv", tmv.tm_year + 1900, tmv.tm_mon + 1,
            tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        FILE* df = nullptr;
        if (_wfopen_s(&df, (OutputDir() + dname).c_str(), L"wb") == 0 && df) {
            fprintf(df,
                "# One row per RunShapeKeyPasses CALL. Join to morph_pass on (frame, ns).\n"
                "# exit:       0 completed, 1 empty-ns, 2 same-frame-repeat (a later draw; the\n"
                "#             passes already ran this frame at the mod's FIRST draw).\n"
                "# READ THIS FIRST when a capture has queued lists and no pass rows:\n"
                "#   no rows at all      -> the drain is never CALLED for that mod (upstream of it)\n"
                "#   pending_in = 0      -> the queue was EMPTY when the draw looked: the game's\n"
                "#                          shapekey dispatch had not been recorded yet this frame.\n"
                "#                          ORDERING, not ownership. Cross-check disp_seen.\n"
                "#   pending_in > 0, matched = 0 -> the ownership filter rejected every list; the\n"
                "#                          queued key and this mod's namespace do not match.\n"
                "# disp_seen:  1 = a shapekey dispatch was recorded this frame before this draw.\n"
                "# vb0/vb6:    sampled at the DRAW. vb0_state set with vb0_shim = 0 means the ini\n"
                "#             named ResourceShapeKeyedPosition and the copy never reached the list.\n"
                "# u0_clear:   0 not attempted, 1 CLEARED, 2 REFUSED (not provably ours), 3 not ready.\n"
                "#             2 means u0 reaches the Loader DIRTY -- that is the kaleidoscope, and\n"
                "#             it happens when u0_identity does not hash to what this mod declares.\n"
                "# u1_clear:   same codes for u1. 0 with the toggle on means this mod published\n"
                "#             no u1 at its dispatch; 2 means the identity size does not hash to a\n"
                "#             ShapeKeyScale value this namespace declares.\n"
                "# u0_identity: what the buffer was BORN as. != u0_bytes means we grew it.\n"
                "#             A value belonging to ANOTHER character is the identity-map poisoning\n"
                "#             (the relocation map is keyed by raw pointer and Windows recycles them).\n");
            fprintf(df,
                "seq\tframe\tns\tmod\texit\tpending_in\tmatched\tleft\tdropped\tran\t"
                "disp_seen\tu0_res\tu0_bytes\tu0_identity\tu0_clear\tu1_clear\treloc_from\treloc_to\t"
                "ps_addr\tvb_last_ps\tvb_binds\tvb_nonnull\t"
                "vb0_state\tvb0_shim\tvb6_state\tvb6_shim\n");
            unsigned dseq = 0;
            for (const DrainRow& dr : drains) {
                const DrainRecord& r = dr.r;
                fprintf(df,
                    "%u\t%u\t%s\t%d\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%llx\t%u\t"
                    "%u\t%u\t%u\t%u\t%u\t"
                    "%llx\t%llx\t%u\t%u\t"
                    "%llx\t%llx\t%llx\t%llx\n",
                    dseq++, r.frame, dr.ns, r.mod_id, (unsigned)r.exit, r.pending_in, r.matched, r.left, r.dropped,
                    r.ran, (unsigned)r.disp_seen, (unsigned long long)r.u0_res, r.u0_bytes, r.u0_identity,
                    (unsigned)r.u0_clear, (unsigned)r.u1_clear, r.reloc_from, r.reloc_to, (unsigned long long)r.ps_addr,
                    (unsigned long long)r.vb_last_ps, r.vb_binds, r.vb_nonnull, (unsigned long long)r.vb0_state,
                    (unsigned long long)r.vb0_shim, (unsigned long long)r.vb6_state, (unsigned long long)r.vb6_shim);
            }
            fclose(df);
            unsigned e0 = 0;
            unsigned e2 = 0;
            unsigned empty_q = 0;
            unsigned no_match = 0;
            for (const DrainRow& dr : drains) {
                if (dr.r.exit == 2) {
                    ++e2;
                    continue;
                }
                if (dr.r.exit != 0) continue;
                ++e0;
                if (!dr.r.pending_in)
                    ++empty_q;
                else if (!dr.r.matched)
                    ++no_match;
            }
            logger::write("MORPHDUMP: wrote %zu DRAIN rows to morph_drain-*.tsv. completed=%u "
                          "same-frame-repeat=%u | OF THE COMPLETED: queue-was-EMPTY=%u "
                          "nothing-matched-this-mod=%u. Those two are the whole question when there are "
                          "no pass rows: empty = ORDERING, unmatched = OWNERSHIP.",
                drains.size(), e0, e2, empty_q, no_match);
        }
    } else {
        logger::write("MORPHDUMP: NO DRAIN ROWS -- RunShapeKeyPasses was never called while armed. The "
                      "failure is UPSTREAM of the drain, in whatever decides to call it.");
    }

    // ---- THE DRAW TABLE: every claimed draw and what its vb6 pointed at ----------------------------
    if (!draws.empty()) {
        wchar_t wname[128];
        swprintf_s(wname, L"\\morph_draw-%04d%02d%02d-%02d%02d%02d.tsv", tmv.tm_year + 1900, tmv.tm_mon + 1,
            tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        FILE* wf = nullptr;
        if (_wfopen_s(&wf, (OutputDir() + wname).c_str(), L"wb") == 0 && wf) {
            fprintf(wf,
                "# One row per CLAIMED DRAW of a shapekey mod, sampled at the vb6 substitution site (RELOCBIND).\n"
                "# vb6_verdict: 0 = no relocation known for the bound resource (the draw reads it AS BOUND),\n"
                "#              1 = SUBSTITUTED with this mod's widened buffer, 2 = DECLINED (the widened\n"
                "#              buffer for that resource belongs to another mod), 3 = slot 6 unbound.\n"
                "# is_depth:    1 = no RTV and a DSV bound (depth/shadow pass); 0 = colour pass; 2 = unknown.\n"
                "# Any verdict other than 1 on a vb6-direct mod means that draw reads whatever the GAME\n"
                "# wrote into the buffer it bound -- for the vanilla u0 that is the unskipped vanilla\n"
                "# Loader/Multiplier output: vanilla deltas indexed by the mod's vertex ids.\n"
                "# ini_null_unbinds: `vbN = null` unbinds the mod's section issued AFTER the verdict was\n"
                "#              sampled. An Applier mod's `vb6 = null` lands here: verdict=1 with\n"
                "#              ini_null_unbinds>0 means the draw reads NO slot 6.\n"
                "# mid_restores: restore_state() calls made FROM the engine's restore callback while\n"
                "#              the section ran (SkeletonMerger dispatches etc. issued BEFORE its drawindexed).\n"
                "# mid_ia_skipped: of those, how many skipped the IA rebinds. It must equal mid_restores: a\n"
                "#              restore that rebinds slot 6 hands the draw the GAME's vanilla morph buffer, so\n"
                "#              verdict=1 with mid_ia_skipped < mid_restores = this draw reads VANILLA deltas at mod ids.\n"
                "# Group by (frame, list) to see which draws share a command list.\n");
            fprintf(wf,
                "seq\tframe\tns\tmod\tcomp\tlist\tindex_count\tfirst_index\tvertex_offset\tis_depth\t"
                "vb6_res\tvb6_va\tvb6_bytes\tvb6_stride\tvb6_verdict\tsub_va\tsub_bytes\tmy_u0\tvb0_res\t"
                "ini_null_unbinds\tmid_restores\tmid_ia_skipped\n");
            unsigned wseq = 0;
            for (const DrawRow& w : draws) {
                const DrawRecord& r = w.r;
                fprintf(wf,
                    "%u\t%u\t%s\t%d\t%d\t%llx\t%u\t%u\t%d\t%u\t"
                    "%llx\t%llx\t%u\t%u\t%u\t%llx\t%u\t%llx\t%llx\t%u\t%u\t%u\n",
                    wseq++, r.frame, w.ns, r.mod_id, r.comp, (unsigned long long)r.list, r.index_count, r.first_index,
                    r.vertex_offset, (unsigned)r.is_depth, (unsigned long long)r.vb6_res, (unsigned long long)r.vb6_va,
                    r.vb6_bytes, r.vb6_stride, (unsigned)r.vb6_verdict, (unsigned long long)r.sub_va, r.sub_bytes,
                    (unsigned long long)r.my_u0, (unsigned long long)r.vb0_res, r.ini_null_unbinds, r.mid_restores,
                    r.mid_ia_skipped);
            }
            fclose(wf);
            unsigned v[4] = {};
            for (const DrawRow& w : draws)
                if (w.r.vb6_verdict < 4) ++v[w.r.vb6_verdict];
            logger::write("MORPHDUMP: wrote %zu DRAW rows to morph_draw-*.tsv. vb6: as-bound=%u substituted=%u "
                          "declined=%u unbound=%u. On a vb6-direct mod anything but 'substituted' reads the "
                          "game's vanilla morph output.",
                draws.size(), v[0], v[1], v[2], v[3]);
        } else {
            logger::write("MORPHDUMP: could not open the draw file -- %zu draw rows lost", draws.size());
        }
    }

    // ---- THE GAME-DRAW TABLE: every game draw of a mod component and what became of it --------------
    if (!gdraws.empty()) {
        wchar_t gname[128];
        swprintf_s(gname, L"\\morph_gamedraw-%04d%02d%02d-%02d%02d%02d.tsv", tmv.tm_year + 1900, tmv.tm_mon + 1,
            tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        FILE* gf = nullptr;
        if (_wfopen_s(&gf, (OutputDir() + gname).c_str(), L"wb") == 0 && gf) {
            fprintf(gf,
                "# One row per GAME draw whose (first_index, index_count) is a mod component, taken in the draw\n"
                "# hook's outermost wrapper: pass identity at ENTRY, outcome at EXIT. ret: 1 = the game's own draw\n"
                "# was suppressed by the hook, 0 = it reached the GPU as the game issued it. claimed: the component\n"
                "# path ran for it. shadow: recognised as a depth/shadow pass by bone VA; cast: that draw triggered\n"
                "# the cast of every component into its target. hidden: dropped at entry (H or hide-by-VS).\n"
                "# injected: DrawIndexedInstanced calls the port issued inside the hook. A pass is (vs, ps, rtv_count,\n"
                "# dsv, vp_w x vp_h). Group by (frame, comp) to list one component's passes side by side.\n"
                "# comp=-1 rows are SAME-MESH draws -- not a component's index range, but vb0 or the IB is one\n"
                "# a claimed draw of that mod used (same_mesh: 1 vb0, 2 IB, 3 both). Never claimed by us.\n");
            fprintf(gf,
                "seq\tframe\tns\tmod\tcomp\tlist\tthread\tindex_count\tfirst_index\tinstance_count\tvs\tps\tpso\t"
                "rtv_count\thas_dsv\tdsv\tpass_gen\tvp_w\tvp_h\tstencil_ref\tstencil_ref_set\tvb0_va\tvb6_va\tvb6_stride\t"
                "claimed\tshadow\tcast\thidden\tinjected\tret\tib_va\tsame_mesh\tcb3_va\tcb4_va\tmerges\n");
            unsigned gseq = 0;
            for (const GameDrawRow& w : gdraws) {
                const GameDrawRecord& r = w.r;
                fprintf(gf,
                    "%u\t%u\t%s\t%d\t%d\t%llx\t%u\t%u\t%u\t%u\t%016llx\t%016llx\t%llx\t"
                    "%u\t%u\t%llx\t%u\t%.0f\t%.0f\t%u\t%u\t%llx\t%llx\t%u\t"
                    "%u\t%u\t%u\t%u\t%u\t%u\t%llx\t%u\t%llx\t%llx\t%u\n",
                    gseq++, r.frame, w.ns, r.mod_id, r.comp, (unsigned long long)r.list, r.thread, r.index_count,
                    r.first_index, r.instance_count, (unsigned long long)r.vs, (unsigned long long)r.ps,
                    (unsigned long long)r.pso, r.rtv_count, (unsigned)r.has_dsv, (unsigned long long)r.dsv, r.pass_gen,
                    r.vp_w, r.vp_h, r.stencil_ref, (unsigned)r.stencil_ref_set, (unsigned long long)r.vb0_va,
                    (unsigned long long)r.vb6_va, r.vb6_stride, (unsigned)r.claimed, (unsigned)r.shadow,
                    (unsigned)r.cast, (unsigned)r.hidden, r.injected, (unsigned)r.ret, (unsigned long long)r.ib_va,
                    (unsigned)r.same_mesh, (unsigned long long)r.cb3_va, (unsigned long long)r.cb4_va,
                    (unsigned)r.merges);
            }
            fclose(gf);
            unsigned proceeded = 0;
            unsigned depth_proceeded = 0;
            unsigned same_mesh = 0;
            unsigned same_mesh_drawn = 0;
            unsigned same_mesh_colour = 0;
            for (const GameDrawRow& w : gdraws) {
                if (w.r.same_mesh) {
                    ++same_mesh;
                    if (!w.r.ret) {
                        ++same_mesh_drawn;
                        if (w.r.rtv_count) ++same_mesh_colour;
                    }
                    continue;
                }
                if (!w.r.ret) {
                    ++proceeded;
                    if (w.r.rtv_count == 0 && w.r.has_dsv) ++depth_proceeded;
                }
            }
            logger::write(
                "MORPHDUMP: wrote %zu GAME-DRAW rows to morph_gamedraw-*.tsv. ret=0 (the game's own draw of a "
                "mod component reached the GPU): %u, of which depth-only %u. Every ret=0 row is vanilla "
                "geometry with the vanilla morph, in that pass. SAME-MESH rows (comp=-1, the character's "
                "vb0/IB drawn through a non-component index range): %u, reached the GPU %u, of which colour %u.",
                gdraws.size(), proceeded, depth_proceeded, same_mesh, same_mesh_drawn, same_mesh_colour);
        } else {
            logger::write("MORPHDUMP: could not open the game-draw file -- %zu rows lost", gdraws.size());
        }
    }

    unsigned owned = 0;
    unsigned unowned = 0;
    unsigned unresolved = 0;
    unsigned unowned_skipped = 0;
    unsigned unowned_fired = 0;
    for (const Row& row : rows) {
        const Record& r = row.r;
        if (r.u0_declared == 1) {
            ++owned;
            continue;
        } else if (r.u0_declared < 0) {
            ++unresolved;
            continue;
        }
        ++unowned;
        if (r.skip) ++unowned_skipped;
        if (r.sections_fired) ++unowned_fired;
    }
    logger::write("MORPHDUMP: wrote %zu rows to morph_dump-*.tsv. owned=%u unowned=%u unresolved=%u | "
                  "OF THE UNOWNED: skipped=%u sections_fired=%u -- both MUST be 0; any non-zero is us "
                  "altering a character no mod owns. unowned=0 would mean the tool saw no unmodded "
                  "dispatch at all, which is a finding about the TOOL, not about the game.",
        rows.size(), owned, unowned, unresolved, unowned_skipped, unowned_fired);
}

} // namespace MorphDump
