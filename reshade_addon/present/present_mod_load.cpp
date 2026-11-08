// present_mod_load.cpp -- see present_mod_load.h.
#include "../addon_common.h"
#include "../addon/addon.h"
#include "../mods/port_section.h"
#include "present_mod_load.h"
#include "../build_stamp.h"
#include "../textures/tex_place.h"
#include "../textures/mod_texture_store.h"
#include "../migoto/port_api.h"
#include "present_toggles.h"

using namespace reshade::api;

ModTexturesLoad g_mod_textures_load;

void ModTexturesLoad::BannerTick()
{
    const unsigned gen = PortApi::LoadGeneration();
    if (banner_gen_ == gen) return;
    banner_gen_ = gen;
    const wchar_t* const fq = L"resource\\wwmiv1\\versionnotification";
    PortApi::BufferResourceInfo bi{};
    if (!PortApi::GetBufferResource(fq, &bi) || !bi.dx12) {
        logger::write("BANNER: WWMI's VersionNotification buffer not found -- its banner stays as shipped");
        return;
    }
    char shipped[64] = {};
    PortApi::BufferInitialData(fq, shipped, (std::min)((size_t)bi.byte_width, sizeof shipped - 1));
    size_t k = 0;
    if (strncmp(shipped, "WWMI ", 5) == 0)
        for (k = 5; shipped[k] && shipped[k] != ' ' && shipped[k] != '\n';) ++k;
    shipped[k] = 0;
    char text[96];
    snprintf(text, sizeof text, "%s\nWWMI-DX12 %s", k ? shipped : "WWMI", WWMI_VERSION);
    const size_t n = strlen(text);
    if (bi.byte_width < n) {
        logger::write("BANNER: VersionNotification is %u B -- '%s' (%zu B) not written", bi.byte_width, text, n);
        return;
    }
    std::vector<char> data(bi.byte_width, 0);
    memcpy(data.data(), text, n);
    if (!PortApi::UploadBufferResource(fq, data.data(), data.size())) {
        logger::write("BANNER: upload failed -- its banner stays as shipped");
        return;
    }
    logger::write("BANNER: WWMI's startup banner now reads '%s' / 'WWMI-DX12 %s'", k ? shipped : "WWMI", WWMI_VERSION);
}

void ModTexturesLoad::Tick()
{
    if (started_) {
        BannerTick();
        const size_t described = g_mod_textures.Resources().size();
        g_mod_textures.SubstantiateWclResources();
        if (g_mod_textures.Resources().size() > described) {
            g_tex_place.ApplyQueued();
            if (g_present_toggles.skip_engine_tick)
                g_tex_place.RetryMissed("new mod textures described");
            else
                g_tex_place.RetryMissedBetweenPasses("new mod textures described");
        }
    }
    if (!started_) {
        started_ = true;
        PortApi::Load(g_addon.wwmi_dir.c_str());
        g_mod_textures.LoadTextureOverrides(); // the SRV heap, sized from what their engine declared
        {
            const unsigned h = PortApi::GameConstantBufferHash(kDx11SkeletonCbByteWidth);
            logger::write("PORT-LEAF skeleton CB: ByteWidth=%u -> hash=%08x %s", kDx11SkeletonCbByteWidth, h,
                h == 0xf02baf77u ? "(matches WWMI MarkBoneDataCB)" : "*** MISMATCH: WWMI expects f02baf77 ***");
        }
        logger::write("PORT-GAP: bare `ps`/`vs` shader-filter operands evaluate to 0 -- their "
                      "process_shader_filter needs a bound-shader leaf we do not have yet");
    }
}
