// uid_hide.cpp -- OURS. See uid_hide.h.
#include "uid_hide.h"
#include <algorithm>
#include <iterator>
#include <string>
#include "../hooks/dxil_psv.h"
#include "../core/dxil_cb.h"
#include "../core/log.h"

namespace {
constexpr float kBig = 3.4e38f;

struct UidPlace {
    bool right; // against the right edge, else the left
    bool bottom; // against the bottom edge, else the top
    float w; // UI pixels of a 1920x1080 screen
    float h;
    float lo[3];
    float hi[3];
};

constexpr UidPlace kPlaces[] = {
    { true, true, 270.72f, 22.68f, { 0.99f, 0.99f, 0.99f }, { kBig, kBig, kBig } },
    { true, true, 270.72f, 22.68f, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f } },
    { false, false, 672.f, 432.f, { 0.792f, 0.698f, -kBig }, { 0.793f, 0.699f, kBig } },
    { false, true, 585.6f, 108.f, { 0.9097f, 0.9097f, 0.9097f }, { 0.9099f, 0.9099f, 0.9099f } },
};

const char* const kTextInputs[] = { "COLOR0", "SV_IsFrontFace0", "SV_Position0", "TEXCOORD0", "TEXCOORD10_centroid0",
    "TEXCOORD11_centroid0" };
}

bool UidHide::IsTextShader(const void* code, size_t size)
{
    DxilShape s;
    if (!code || !DxilShaderShape(code, size, s)) return false;
    if (s.samplers != 2 || s.cbuffers != 4 || s.typed_srvs != 2 || s.other_resources != 0) return false;
    if (s.outputs.size() != 1 || s.outputs[0] != "SV_Target0") return false;
    std::sort(s.inputs.begin(), s.inputs.end());
    if (!std::equal(s.inputs.begin(), s.inputs.end(), std::begin(kTextInputs), std::end(kTextInputs))) return false;
    std::vector<DxilCbuf> cbs;
    if (!DxilCbuffers(code, size, cbs)) return false;
    for (const DxilCbuf& c : cbs)
        if (c.space == 0 && c.reg == 3) return c.bytes == 48;
    return false;
}

std::vector<RfxDxilCut> UidHide::Cuts(uint32_t target)
{
    const float w = (float)(target >> 16);
    const float h = (float)(target & 0xFFFF);
    const float ui = (std::min)(w / 1920.f, h / 1080.f);
    std::vector<RfxDxilCut> out;
    for (const UidPlace& p : kPlaces) {
        RfxDxilCut c{};
        c.x0 = p.right ? w - p.w * ui : 0.f;
        c.x1 = p.right ? w : p.w * ui;
        c.y0 = p.bottom ? h - p.h * ui : 0.f;
        c.y1 = p.bottom ? h : p.h * ui;
        std::copy(std::begin(p.lo), std::end(p.lo), c.lo);
        std::copy(std::begin(p.hi), std::end(p.hi), c.hi);
        out.push_back(c);
    }
    return out;
}

void UidHide::NoteBackBuffer(reshade::api::swapchain* sc)
{
    const reshade::api::resource_desc d = sc->get_device()->get_resource_desc(sc->get_current_back_buffer());
    const uint32_t t = (d.texture.width & 0xFFFFu) << 16 | (d.texture.height & 0xFFFFu);
    if (target_.exchange(t, std::memory_order_acq_rel) != t)
        logger::write(
            "UIDCUT: back buffer %ux%u -- the UID boxes are cut for this size", d.texture.width, d.texture.height);
}
