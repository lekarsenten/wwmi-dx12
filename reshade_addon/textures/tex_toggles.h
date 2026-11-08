#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "../instrumentation/diag_gate.h"

struct TexToggles {
    bool disable = false; // C1: game textures on every draw
    bool placement_swap = true; // C2, PIPELINE 1: placement-time swap (copy/update desc hooks)
    bool inplace_stamp = true; // C3: per-draw in-place stamp, live heap
    bool read_slots = true; // C3R: C3 stamps only the slots the draw's shaders read
    bool lineage = true;
    bool lod_alias_off = false;
    bool mip_exact = true;
    bool upload_gpu = false; // D16: every texture upload hashed from a GPU copy, checked against the processor's read
    bool dump_degenerates = false; // D15: each degenerate LOD low as <dump dir>\degenerates\<hash>.dds
};
extern TexToggles g_tex_toggles;

struct TexStats {
    std::atomic<uint32_t> lineage_count{ 0 };
    std::atomic<uint32_t> inject_count{ 0 }; // per-draw swaps (claimed draws)
    std::atomic<uint32_t> global_count{ 0 }; // descriptor-placement swaps
    std::atomic<uint32_t> global_miss{ 0 }; // placements with a rule but an EMPTY selection (GTEXMISS)
    std::atomic<unsigned> off_leaks{ 0 }; // slot reads by draws not handed back
    std::atomic<unsigned> off_kept{ 0 }; // hand-backs that left the mod texture in place
    // Upload hashing, for the performance report.
    std::atomic<uint64_t> hash_all{ 0 };
    std::atomic<uint64_t> hash_rehash{ 0 };
    std::atomic<uint64_t> hash_same{ 0 };
    std::atomic<uint64_t> hash_bytes{ 0 };
    // The arrival paths (TEXSTREAM).
    std::atomic<uint32_t> ts_tex2tex{ 0 }; // on_copy_texture_region calls past the asset gate
    std::atomic<uint32_t> ts_stream{ 0 }; // ...same format, no box, same mip, different size (a stream copy)
    std::atomic<uint32_t> ts_stream_nobig{ 0 }; // ...whose bigger side had no hash yet (LineageAlias could not alias)
    std::atomic<uint32_t> ts_buf_mip0{ 0 }; // buffer->texture mip0 uploads hashed
    std::atomic<uint32_t> ts_buf_mipk{ 0 }; // buffer->texture mip K uploads past the floor
    std::atomic<uint32_t> ts_mipk_target{ 0 }; // ...on a resource whose mip0 is a target (recorded or already known)
    std::atomic<uint32_t> ts_mipk_held{ 0 }; // ...held because mip0 was not known yet
    std::atomic<uint32_t> ts_held_resolved{ 0 }; // held entries resolved at mip0 (recorded or not a target)
    std::atomic<uint32_t> ts_target_mip0{ 0 }; // mip0 identifications of a rule target
    std::atomic<uint32_t> ts_rb_late{ 0 }; // readbacks whose fence went in a second after the submission
    std::atomic<uint32_t> ts_up_gpu{ 0 }; // buffer->texture uploads hashed from a GPU copy
    std::atomic<uint32_t> ts_up_same{ 0 }; // ...whose hash matched the processor's read (D16)
    std::atomic<uint32_t> ts_up_diff{ 0 }; // ...and did not
    std::atomic<uint32_t> ts_rb_own{ 0 }; // readbacks that did not fit the shared buffer and created their own
    std::atomic<uint64_t> ts_rb_own_us{ 0 }; // ...time spent creating them, on the game's threads
    std::atomic<uint32_t> ts_rb_waits{ 0 }; // readbacks that waited 2 ms or more for the lock
    std::atomic<uint32_t> ts_rb_reused{ 0 }; // readbacks that took a spare own buffer instead of creating one
};
extern TexStats g_tex_stats;

inline constexpr int kTexBudgetSteps = 6;
inline const char* TexBudgetName(int step)
{
    static const char* const names[] = { "Original", "Auto", "1024 MB", "512 MB", "256 MB", "128 MB" };
    return step >= 0 && step < kTexBudgetSteps ? names[step] : names[0];
}
inline unsigned TexBudgetMb(int step) // 0 = no per-mod ceiling
{
    return step >= 2 && step < kTexBudgetSteps ? 1024u >> (step - 2) : 0u;
}
inline bool TexCollects(int step) // the collector (tex_gc) runs
{
    return step >= 1 && step < kTexBudgetSteps;
}
