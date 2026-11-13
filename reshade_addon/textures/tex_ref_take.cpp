// The reference take. See tex_ref_take.h.
#include "tex_ref_take.h"
#include "tex_identity.h"
#include "tex_lod.h"
#include "tex_look.h"
#include "tex_look_sig.h"
#include "tex_upload_identity.h"
#include "../migoto/dx12_dump_readback.h"
#include "../migoto/port_api.h"
#include "../core/log.h"
#include <cstdio>

TexRefTake g_tex_ref_take;

void TexRefTake::Arm()
{
    std::lock_guard l(mutex_);
    if (collecting_.load(std::memory_order_relaxed)) return;
    until_ = g_frame_id + 2;
    last_ = "collecting the textures character draws read";
    collecting_.store(true, std::memory_order_relaxed);
    logger::write("TEXREFS: collecting the textures game character draws read, frames %u to %u", g_frame_id, until_);
}

bool TexRefTake::Collecting() const
{
    return collecting_.load(std::memory_order_relaxed);
}

void TexRefTake::Note(ID3D12Resource* res)
{
    const uint32_t hash = g_tex_identity.HashOf(reinterpret_cast<uint64_t>(res));
    if (!hash) return;
    std::lock_guard l(mutex_);
    if (!collecting_.load(std::memory_order_relaxed)) return;
    if (seen_.emplace(res, hash).second) res->AddRef();
}

void TexRefTake::Tick()
{
    std::unordered_map<ID3D12Resource*, uint32_t> seen;
    {
        std::lock_guard l(mutex_);
        if (!collecting_.load(std::memory_order_relaxed) || g_frame_id <= until_) return;
        collecting_.store(false, std::memory_order_relaxed);
        seen.swap(seen_);
    }
    Take(seen);
    for (const auto& kv : seen) kv.first->Release();
}

void TexRefTake::Take(const std::unordered_map<ID3D12Resource*, uint32_t>& seen)
{
    MigotoPort::PreDrainBegin();
    for (const auto& kv : seen) MigotoPort::PreDrainAdd(kv.first);
    MigotoPort::PreDrainFlush();
    unsigned kept = 0;
    unsigned offered = 0;
    unsigned ineligible = 0;
    unsigned not_copied = 0;
    unsigned other_content = 0;
    for (const auto& [res, hash] : seen) {
        unsigned bytes = 0;
        unsigned pitch = 0;
        const auto* data = static_cast<const uint8_t*>(MigotoPort::PreDrainLookup(res, &bytes, &pitch));
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        TexAssetLayout L;
        if (!data || !TexAssetLayoutOf(d, 0, L) || !L.rows || pitch < L.tight_row ||
            bytes < (size_t)pitch * (L.rows - 1) + L.tight_row) {
            ++not_copied;
            continue;
        }
        const std::vector<uint8_t> tight = TightFromUpload(data, pitch, L);
        const uint32_t crc = TexIdentityHash(PortApi::Crc32c(0, tight.data(), tight.size()), L.w, L.h,
            d.MipLevels ? d.MipLevels : 1, d.DepthOrArraySize ? d.DepthOrArraySize : 1, (uint32_t)d.Format);
        if (crc != hash) {
            ++other_content;
            continue;
        }
        const TexLookCut cut = TexLookCutOf(tight.data(), tight.size(), L.w, L.h, (uint32_t)d.Format);
        if (cut.blocks.empty()) {
            ++ineligible;
            continue;
        }
        const uint32_t known = g_tex_lod.Known(hash);
        ++offered;
        if (g_tex_look.TakeRef(hash, known ? known : hash, cut)) ++kept;
    }
    MigotoPort::PreDrainBegin();
    char b[384];
    std::snprintf(b, sizeof b,
        "%zu texture(s) read by character draws: %u new or denser reference(s); %u already had one as dense or are "
        "too flat; %u of a size or format signatures skip; %u not copied; %u whose copy hashes otherwise",
        seen.size(), kept, offered - kept, ineligible, not_copied, other_content);
    logger::write("TEXREFS: %s", b);
    std::lock_guard l(mutex_);
    last_ = b;
}

std::string TexRefTake::Last() const
{
    std::lock_guard l(mutex_);
    return last_;
}

std::string TexRefTake::Command(const std::vector<std::string>& args)
{
    if (args.size() == 1 && args[0] == "take") {
        Arm();
        return "collecting for two frames; texrefs shows the result";
    }
    if (!args.empty()) return "ERR texrefs take, or texrefs for the last result";
    const std::string last = Last();
    return last.empty() ? "no take yet" : last;
}
