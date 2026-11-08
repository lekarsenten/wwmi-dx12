#pragma once
// The parts of the draw path (draw_hook.cpp on_draw_indexed_body), in the order it calls them.
#include "draw_context.h"
#include <optional>
#include <reshade.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include "../hooks/descriptor_shadow.h"
#include "../instrumentation/diag_gate.h"

struct ResInfo;
namespace PortApi {
class DrawListScope;
}

// [ShaderOverride*] by VS / PS / GS hash, and the post lists; true = the ini's handling=skip (draw_claim.cpp).
bool RunDrawShaderOverrides(const DrawArgs& da);
// A small unskinned draw whose vertex buffer hashes to a variable-only section runs it, as DX11's
// `checktextureoverride = vb0` would; true = the section skips the draw (draw_claim.cpp).
bool RunSmallMeshOverrides(const DrawArgs& da);
#ifdef WWMI_DEBUG
// Every small draw RunSmallMeshOverrides saw, one line per vertex buffer; count = 0 for all (debug pipe `smallmesh`).
std::string SmallMeshReport(uint32_t count);
// What checked draws' sections left at the registers they wrote, newest first; game_hash 0 = all (debug pipe
// `ovrpicks`). Each line counts the changes since the last query.
std::string OverridePickReport(uint32_t game_hash);
#endif
// Which mod component this draw is; a value = return it from the hook (a mesh-hash skip) (draw_claim.cpp).
std::optional<bool> ClaimDraw(DrawContext& ctx);
void DrawProbesClaim(DrawContext& ctx); // draw_probes.cpp
void LodPinNote(DrawContext& ctx); // draw_claim.cpp
void DrawProbesPasses(DrawContext& ctx); // draw_probes.cpp
// A character depth pass the mod components are to be cast into (draw_shadow.cpp).
void RecogniseShadowPass(DrawContext& ctx);
void DrawProbeVb6Reader(DrawContext& ctx); // draw_probes.cpp
void DrawProbeOffLeak(DrawContext& ctx); // draw_probes.cpp
void DrawProbeUnstamp(const ResInfo& ri, bool restored); // draw_probes.cpp
// The frame-analysis hand-off of a game draw, CDRAW, DRAW, LODDIAG; after the claim (draw_diag.cpp, debug builds).
WWMI_DEBUG_FN void DrawDiagIndexed(DrawContext& ctx) WWMI_DEBUG_BODY();
void StampClaimedTextures(DrawContext& ctx); // draw_texswap.cpp
// The game's bindings at this draw for the vs/ps registers a section reads and does not set: its descriptors for the
// section's draws, its textures published for `checktextureoverride` and the like (draw_restore.cpp).
void InheritGameBindings(PortApi::DrawListScope& scope, reshade::api::command_list* cmd_list);
void StampOverrideSrvs(reshade::api::command_list* cmd_list, uint32_t srv_gen0);
// `bb` for a section drawing on the game's list: the game's render target when it is a swap-chain buffer
// (draw_restore.cpp).
void GameBackBuffer(PortApi::DrawListScope& scope, reshade::api::command_list* cmd_list, const CmdRT& rt);
// After a [ShaderOverride] section drew on the game's list: the game's PSO, heaps, root signature and arguments,
// targets, buffers and fixed state put back as far as the draw changed them; `user` is the command_list
// (draw_restore.cpp).
void RestoreOverrideDraw(void* user);
// The game's own descriptors back in every stamped slot of this draw's tables; how many (draw_texswap.cpp).
unsigned UnstampClaimedTextures(const DrawArgs& da);
// The claimed draw replaced by the mod's geometry; the hook's return value (draw_inject.cpp).
bool InjectClaimedDraw(DrawContext& ctx);
