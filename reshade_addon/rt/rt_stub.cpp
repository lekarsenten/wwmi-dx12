// rt/rt_stub.cpp -- the RT module left out (WWMI_RT=0): every hook the core calls does no RT work. The core then
// behaves as it does with ray tracing off: no ring is ever recorded, so every RT branch in it is untaken.
#include <imgui.h>
#include "rt/rt_hooks.h"
#include "rt/rt_rings.h"
#include "rt/rt_pal_record.h"
#include "rt/rt_diag_dispatch.h"
#include "rt/rt_blas.h"
#include "rt/rt_draw.h"

RtDrawVbs g_rt_draw_vbs;
RtSkinRings g_rt_rings;
RtPalRecorder g_rt_pal_recorder;
RtSkinRead g_rt_skin_read;
RtBonesLog g_rt_bones;
RtBlasHook g_rt_blas;
RtDrawPass g_rt_draw;

void RtDrawVbs::Note(int, int, const RtDrawVb&) {}
bool RtSkinRings::Overlaps(uint64_t, uint64_t) const
{
    return false;
}
size_t RtSkinRings::Count() const
{
    return 0;
}
void RtSkinRings::ForgetRange(uint64_t, uint64_t) {}
void RtPalRecorder::Record(reshade::api::command_list*, const CmdState&, uint64_t) {}
bool RtBonesLog::WindowOpen() const
{
    return false;
}
void RtBonesLog::Log(reshade::api::command_list*, const CmdState&, uint32_t, uint64_t, bool) {}
void RtSkinRead::Drain() {}
void RtSkinRead::Record(reshade::api::command_list*, const CmdState&, uint64_t, uint32_t) {}
bool RtBlasHook::OnBuild(reshade::api::command_list*, reshade::api::acceleration_structure_type,
    reshade::api::acceleration_structure_build_flags, uint32_t, const reshade::api::acceleration_structure_build_input*,
    reshade::api::resource_view, reshade::api::acceleration_structure_build_mode, unsigned long long)
{
    return false;
}
VfRestore RtDrawPass::SkinDraw(reshade::api::command_list*, ID3D12GraphicsCommandList*, int, const WwmiMod::Component*,
    uint32_t, uint32_t, uint32_t, uint32_t, int32_t, uint64_t, uint64_t, int, bool)
{
    return {};
}
RtPalResult RtDrawPass::PalPublish(uint64_t, int, const WwmiMod::Component*, uint64_t)
{
    return {};
}
void RtDrawOverlay()
{
    ImGui::TextDisabled("  ray tracing: this build has no RT module (WWMI_RT=0)");
}
bool RtModulePresent()
{
    return false;
}
