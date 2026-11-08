#pragma once
#include <atomic>
#include <cstdint>
#include "../migoto/prof_core.h" // ProfScope / PROF, the accumulators, ProfGpuWork, ProfLibStep

enum ProfBucket {
    PROF_DescUpdate = 0, // on_update_descriptor_tables (TexPlacement::Swap per SRV placement)
    PROF_DescCopy, // on_copy_descriptor_tables   (TOP-LEVEL total)
    PROF_CopyBufToTex, // on_copy_buffer_to_texture   (upload identity hash)
    PROF_CB_Alloc, //   - the tight mip0 buffer: allocation + zero fill (nests wherever an upload is hashed)
    PROF_CB_Copy, //   - its rows read out of the upload heap (write-combined)
    PROF_CB_Crc, //   - the crc32c over it
    PROF_CB_Map, //   - Map/Unmap of the upload buffer
    PROF_CB_TexLod, //   - TexLook::OnCut: the patch decode + reference scan
    PROF_CopyTexRegion, // on_copy_texture_region      (mip-shift lineage propagation)
    PROF_Barrier, // on_barrier                  (resource-state tracking)
    PROF_BindDescTables, // on_bind_descriptor_tables
    PROF_BindPipeline, // on_bind_pipeline
    PROF_DrawIndexed, // on_draw_indexed             (geometry claim / mesh inject) -- TOP-LEVEL total
    PROF_DI_TexScan, //   - per-draw 128-slot descriptor/tex-hash scans (TEXOWN + claim-record + char_tex)
    PROF_DI_RfxDecide, //   - RabbitFX SetTextures decide + bind (shadow-heap mirror)
    PROF_DI_ClaimInject, //   - claim decision + skeleton/bone-CB capture bookkeeping
    PROF_DI_BoneHash, //     - HashBoneCB: skeleton-content hash (nests wherever called -- ClaimInject, LOD pin, probes).
    PROF_DI_Inject, //   - post-claim: skin CB bind + PSO + replacement DrawIndexedInstanced replays
    PROF_DI_ShapeKeys, //     - RunShapeKeyPasses TOTAL (once per mod per frame, inside Inject)
    PROF_SK_OwnBuf, //       - the ResourceTracker declared-hash scan (stable per-mod cs-u0 bind)
    PROF_SK_Lists, //       - RunResolvedCommandList x N (Setup/Load/Multiply/Apply)
    PROF_SK_Cb0, //         - ShapekeyStore::GetSnap + PushGameConstantBuffer (this pass's own cb0)
    PROF_SK_Run, //         - PortApi::RunResolvedCommandList: THEIR interpreter under EngineLock; IssueDispatch nests
    PROF_SK_Lock, //           - EngineLock acquisition WAIT inside RunResolvedCommandList (contention with the dispatch hook)
    PROF_SK_Row, //         - the SKPASS row: PipeState reads, GetVarNs x3, GetIniParams, MorphDump record, log
    PROF_DI_PortSect, //     - PortRunSection: THEIR ini command list for this component (inside Inject)
    PROF_DI_PsLookup, //       - RunSectionByName: the fq name -> section lookup (Widen + map find), per draw
    PROF_DI_PsDraw, //       - the section's drawindexed callback: the addon's injected draw (hk.drawindexed)
    PROF_PS_Observe, //       - TypedTrampoline: registry keys narrowed + the addon's typed hooks, per command
    PROF_PS_Trace, //       - CommandTrampoline: ini_line narrowed per command (H3 trace / CHAIN log only)
    PROF_DI_ShadowCast, //     - the whole is_full_shadow_pass cast: N mods x N components per depth target
    PROF_DI_Restore, //     - restore_state: 5 VBs + IB + bone CBVs, on EVERY claimed draw
    PROF_DI_TexHash, //     - hash-keyed texture override walk on a claimed draw (bound tables x 128 slots)
    PROF_TH_Walk, //       - per bound table: heap offset + the 128-slot res/hash resolve (TexSelector::ResolveBound)
    PROF_TH_Select, //       - per bound table, its slot loop: selection (TexSelector::SelectIdx), LOD alias, learned-slot replay, stamp
    PROF_TexSel_Engine, //         - SelectTextureOverride for a hash that HAS a section: engine lock + their list run.
    //           Nests wherever a selection runs (TH:Select, copy hooks)
    PROF_TexSel_Lock, //           - EngineLock acquisition WAIT in it
    PROF_TH_Detect, //       - $object_detected = 1 for the claimed mod, then its texture-timing note
    PROF_TH_DetLock, //         - EngineLock acquisition WAIT, only when the flag is not already 1
    PROF_TH_Prep, //       - the table walk's setup: descriptor size, bound tables, scan lengths, read slots
    PROF_DI_VbPrep, //     - VB/IB snapshot, ApplyVB, restore_state setup, cloth/deform/shadow-table binds
    PROF_DI_DrawComp, //     - draw_comp, one component (claimed draw, or each component of a shadow cast). SELF =
    //       the section's RAII teardown (EngineScope unlock, DrawListScope, PushCbGuard, PortRestore)
    PROF_DI_CompSetup, //       - BEFORE PortSect: section name, hooks, EngineScope, PortRestore snapshot,
    //         CB/VB push, RELOCBIND (DI:ShapeKeys nests here at runtime)
    PROF_DI_EngLock, //         - EngineScope acquisition WAIT (engine lock, then held across the whole section)
    PROF_CS_Prep, //         - section name, hooks, the section-draw state: before the lock
    PROF_CS_Restore, //         - PortRestore snapshot (layout tables, heaps, PSO), DrawListScope
    PROF_CS_Push, //         - CB / VB pushes, RtPal, bone CB records, shadow bone, shapekey passes
    PROF_CS_Reloc, //         - relocated VB / IB binds, the MorphDump draw row and the SKCAP vb6 snapshot
    PROF_CS_SectTex, //         - PublishSectionTextures
    PROF_DI_CompPost, //       - AFTER PortSect: mid-restore / MorphDump draw rows, CB restore, counters, fallback restore
    //       (DI:PortSect is a child of DrawComp at runtime; its row sits above, unmoved)
    PROF_DI_Tail, //     - the legacy tail: MERGEDDRAW, GetDrawOverrides, replacement DrawIndexedInstanced
    PROF_DI_Prologue, //     - EVERY draw pays this: from the top of Inject to the claimed branch
    PROF_DI_Probe, //       - the unconditional probe/budget block (CharacterRecognition lookups + set ops)
    PROF_DI_Vb6Reader, //       - the slot-6 relocation lookup on every draw while debug is on (g_buffer_reloc.Resolve)
    PROF_DI_PsCarry, //   - CarryPsReads: every character draw's ps-t0..t8 noted for the list (DX11's persisting slots)
    PROF_CmdLock, //   - the null-list mutex acquisition wait (nested inside whatever scope was running)
    PROF_Dispatch, // on_dispatch                 (shapekey ShaderOverride + the whole cs-u0 chain)
    PROF_CsU0, //   - cs-u0 resolve + validate + publish (only on shapekey dispatches)
    PROF_Disp_Lock, //   - EngineLock acquisition WAIT in DispatchShaderOverride (the draw hook calls it too)
    PROF_Disp_Resolve, //   - SkResolveU0, every dispatch
    PROF_Disp_Ovr, //   - RunDispatchOverrides (their [ShaderOverride] lookup + sections)
    PROF_Disp_Learn, //   - LearnSkinCache, every dispatch
    PROF_IssueDispatch, // MigotoPort::IssueDispatch (their compute executor: CollectBinds/RootSig/PSO/
    // BindDescriptors/Dispatch). SUB: it always nests in whatever ran the list.
    PROF_IssueDraw, // MigotoPort::IssueDraw -- the sibling executor for draws their engine issues itself
    PROF_UploadQueue, //   - QueueUpload / QueueTextureUpload: MakeStaging + Map + memcpy
    PROF_UploadFlush, //   - FlushPendingUploads: copies + transitions + staging retire
    PROF_Present, // on_present                  (TOTAL frame-boundary work)
    PROF_DrainTexHash, //   - texture readback + BLOCKING fence
    PROF_EngineTick, //   - g_mod_textures.Tick + the PORT's [Present] command lists + UI + presence tick
    PROF_ET_TexStore, //     - SubstantiateWclResources: the per-frame walk describing substantiated mod textures
    PROF_ET_Begin, //     - the [Present] list's Begin: ring fence wait + allocator/list reset
    PROF_ET_Lists, //     - their [Present] pre + input + post, and the per-mod var reads between
    PROF_ET_Submit, //     - flush immediate list + pending uploads + back-buffer restore + Close/Execute/Signal
    PROF_ET_Presence, //     - PresenceTick + RunPendingPost
    PROF_PresTail, //   - on_present tail: shapekey/ibhash-drain/diag
    PROF_IbHashDrain, //     - IbHash::DrainQueue (GPU IB-hash anchor)
    PROF_DiagPresent, //     - the present's debug-only dumps and captures
    // The hooks that had no bucket: each is a per-call tax the game's recording threads pay.
    PROF_BindVB, // on_bind_vertex_buffers (VA + width per buffer, from the runtime)
    PROF_BindIB, // on_bind_index_buffer
    PROF_BindRT, // on_bind_render_targets
    PROF_PushDesc, // on_push_descriptors (root CBV/SRV/UAV: view -> resource, VA, width)
    PROF_CmdReset, // on_reset_command_list
    PROF_CmdClose, // on_close_command_list (our buffers back to COMMON, removal pin ring)
    PROF_CmdExecute, // on_execute_command_list (removal pin check + ring)
    PROF_CopyBufRegion, // on_copy_buffer_region (IbHash forget)
    PROF_InitView, // on_init_resource_view
    PROF_COUNT
};

enum PerfArea {
    PA_Find = 0,
    PA_Draw,
    PA_DrawScripts,
    PA_ShapeKeys,
    PA_ModTex,
    PA_GameTex,
    PA_Scripts,
    PA_Uploads,
    PA_Follow,
    PA_Wait,
    PA_Diag,
    PA_COUNT
};
inline constexpr const char* kPerfAreaNames[PA_COUNT] = {
    "Finding modded characters",
    "Drawing modded characters",
    "Mod scripts at each draw",
    "Shape keys and skeletons",
    "Putting mod textures on",
    "Recognising game textures",
    "Mod scripts once a frame",
    "Sending mod data to the graphics card",
    "Following the game's rendering",
    "Waiting for other threads",
    "Debug log and captures",
};

struct ProfStep {
    const char* name;
    PerfArea area;
};
inline constexpr ProfStep kProfSteps[PROF_COUNT] = {
    { "DescUpdate", PA_ModTex },
    { "DescCopy", PA_Follow },
    { "CopyBufToTex", PA_GameTex },
    { "  CB:Alloc", PA_GameTex },
    { "  CB:Copy", PA_GameTex },
    { "  CB:Crc", PA_GameTex },
    { "  CB:Map", PA_GameTex },
    { "  CB:TexLod", PA_GameTex },
    { "CopyTexRegion", PA_GameTex },
    { "Barrier", PA_Follow },
    { "BindDescTables", PA_Follow },
    { "BindPipeline", PA_Follow },
    { "DrawIndexed", PA_Find },
    { "  DI:TexScan", PA_Find },
    { "  DI:RfxDecide", PA_ModTex },
    { "  DI:ClaimInject", PA_Find },
    { "    DI:BoneHash", PA_Find },
    { "  DI:Inject", PA_Draw },
    { "    DI:ShapeKeys", PA_ShapeKeys },
    { "      SK:OwnBuf", PA_ShapeKeys },
    { "      SK:Lists", PA_ShapeKeys },
    { "        SK:Cb0", PA_ShapeKeys },
    { "        SK:Run", PA_ShapeKeys },
    { "          SK:Lock", PA_Wait },
    { "        SK:Row", PA_ShapeKeys },
    { "    DI:PortSect", PA_DrawScripts },
    { "      DI:PsLookup", PA_DrawScripts },
    { "      DI:PsDraw", PA_Draw },
    { "      PS:Observe", PA_DrawScripts },
    { "      PS:Trace", PA_Diag },
    { "    DI:ShadowCast", PA_Draw },
    { "    DI:Restore", PA_Draw },
    { "    DI:TexHash", PA_ModTex },
    { "      TH:Walk", PA_ModTex },
    { "      TH:Select", PA_ModTex },
    { "        TexSel:Engine", PA_ModTex },
    { "          TexSel:Lock", PA_Wait },
    { "      TH:Detect", PA_Find },
    { "        TH:DetLock", PA_Wait },
    { "      TH:Prep", PA_ModTex },
    { "    DI:VbPrep", PA_Draw },
    { "    DI:DrawComp", PA_Draw },
    { "      DI:CompSetup", PA_Draw },
    { "        DI:EngLock", PA_Wait },
    { "        CS:Prep", PA_Draw },
    { "        CS:Restore", PA_Draw },
    { "        CS:Push", PA_Draw },
    { "        CS:Reloc", PA_Draw },
    { "        CS:SectTex", PA_ModTex },
    { "      DI:CompPost", PA_Draw },
    { "    DI:Tail", PA_Draw },
    { "    DI:Prologue", PA_Find },
    { "      DI:Probe", PA_Find },
    { "      DI:Vb6Reader", PA_Diag },
    { "  DI:PsCarry", PA_ModTex },
    { "  CmdLock:wait", PA_Wait },
    { "Dispatch", PA_ShapeKeys },
    { "  CsU0", PA_ShapeKeys },
    { "  Disp:Lock", PA_Wait },
    { "  Disp:Resolve", PA_ShapeKeys },
    { "  Disp:Ovr", PA_ShapeKeys },
    { "  Disp:Learn", PA_ShapeKeys },
    { "IssueDispatch", PA_ShapeKeys },
    { "IssueDraw", PA_Draw },
    { "  UploadQueue", PA_Uploads },
    { "  UploadFlush", PA_Uploads },
    { "Present(total)", PA_Scripts },
    { "  DrainTexHash", PA_GameTex },
    { "  EngineTick", PA_Scripts },
    { "    ET:TexStore", PA_Scripts },
    { "    ET:Begin", PA_Scripts },
    { "    ET:Lists", PA_Scripts },
    { "    ET:Submit", PA_Scripts },
    { "    ET:Presence", PA_Scripts },
    { "  PresTail", PA_Scripts },
    { "    PT:IbHashDrain", PA_Find },
    { "    PT:DiagPresent", PA_Diag },
    { "BindVB", PA_Follow },
    { "BindIB", PA_Follow },
    { "BindRT", PA_Follow },
    { "PushDesc", PA_Follow },
    { "CmdReset", PA_Follow },
    { "CmdClose", PA_Follow },
    { "CmdExecute", PA_Follow },
    { "CopyBufRegion", PA_Follow },
    { "InitView", PA_Follow },
};
static_assert(sizeof(kProfSteps) / sizeof(kProfSteps[0]) == PROF_COUNT,
    "kProfSteps is out of sync with ProfBucket -- rows would be MISLABELLED, which is worse "
    "than a missing row: every reading after the gap names the wrong step.");

static_assert(PROF_COUNT <= kProfMaxBuckets, "prof_core.h's accumulators are smaller than the bucket list.");

// The lib's steps (prof_core.h) bound to their rows. Once, at AddonInit, before the profiler can be enabled.
inline void ProfBindLibSteps()
{
    g_prof_lib_bucket[PL_UploadQueue] = PROF_UploadQueue;
    g_prof_lib_bucket[PL_UploadFlush] = PROF_UploadFlush;
    g_prof_lib_bucket[PL_IssueDraw] = PROF_IssueDraw;
    g_prof_lib_bucket[PL_IssueDispatch] = PROF_IssueDispatch;
    g_prof_lib_bucket[PL_SK_Lock] = PROF_SK_Lock;
    g_prof_lib_bucket[PL_Disp_Lock] = PROF_Disp_Lock;
    g_prof_lib_bucket[PL_DI_PsLookup] = PROF_DI_PsLookup;
    g_prof_lib_bucket[PL_PS_Trace] = PROF_PS_Trace;
    g_prof_lib_bucket[PL_PS_Observe] = PROF_PS_Observe;
    g_prof_lib_bucket[PL_TexSel_Engine] = PROF_TexSel_Engine;
    g_prof_lib_bucket[PL_TexSel_Lock] = PROF_TexSel_Lock;
}
