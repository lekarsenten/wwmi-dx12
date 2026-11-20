// A22: a mod's shape-key batch numbers answered with the game's on DX12. See sk_batch_fix.h.
#include "sk_batch_fix.h"
#include "sk_toggles.h"
#include "../core/log.h"
#include "../migoto/command_observer.h"
#include "../migoto/port_api.h"
#include <cwchar>
#include <mutex>
#include <set>
#include <tuple>

namespace {

enum Tag : int {
    kOffset0,
    kOffset1,
    kDispatch0,
    kDispatch1,
    kTagCount,
};

const wchar_t* const kTagNames[kTagCount] = {
    L"$\\wwmiv1\\shapekey_vertex_offset_original_batch0",
    L"$\\wwmiv1\\shapekey_vertex_offset_original_batch1",
    L"$\\wwmiv1\\shapekey_dispatch_size_y_original_batch0",
    L"$\\wwmiv1\\shapekey_dispatch_size_y_original_batch1",
};

const wchar_t* const kChecksumNames[2] = {
    L"shapekey_checksum_batch0",
    L"shapekey_checksum_batch1",
};

thread_local const SkBatchPass* t_pass = nullptr;

int Classify(const std::wstring& name)
{
    for (int t = 0; t < kTagCount; ++t)
        if (_wcsicmp(name.c_str(), kTagNames[t]) == 0) return t;
    return -1;
}

const bool s_assign_hook =
    (MigotoPort::CommandObserverInstance().SetAssignHook(&Classify, &SkBatchPass::OnAssigned), true);

} // namespace

GameBatch GameBatchOf(const uint32_t row0[4], const uint32_t row65[4])
{
    GameBatch b;
    b.checksum = row0[0] + row0[1] + row0[2] + row0[3];
    b.vertex_offset = row65[1];
    return b;
}

SkBatchPass::SkBatchPass(const GameBatch* game, unsigned tgy, const std::wstring& ns)
    : outer_(t_pass)
    , game_(game ? *game : GameBatch{})
    , have_(game != nullptr)
    , tgy_(tgy)
    , ns_(ns)
{
    t_pass = this;
}

SkBatchPass::~SkBatchPass()
{
    t_pass = outer_;
}

void SkBatchPass::OnAssigned(int tag, float* var)
{
    const SkBatchPass* p = t_pass;
    if (!p || !p->have_ || !g_sk_toggles.game_batches || !var || tag < 0 || tag >= kTagCount) return;
    const int batch = (tag == kOffset1 || tag == kDispatch1) ? 1 : 0;
    const bool offset = tag == kOffset0 || tag == kOffset1;
    const float checksum = PortApi::GetVarNs(L"WWMIv1", kChecksumNames[batch], -1.f);
    if (checksum < 0.f || (uint32_t)checksum != p->game_.checksum) return;
    const float game = offset ? (float)p->game_.vertex_offset : (float)p->tgy_;
    if (*var == game) return;
    const float was = *var;
    *var = game;
    p->Note(batch, offset, was, game);
}

void SkBatchPass::Note(int batch, bool offset, float was, float now) const
{
    static std::mutex mtx;
    static std::set<std::tuple<std::wstring, int, bool>> said;
    {
        std::lock_guard l(mtx);
        if (!said.emplace(ns_, batch, offset).second) return;
    }
    logger::write("SKBATCH ns='%ls' batch %d: %s %.0f -> %.0f, the game's on DX12 (batch checksum %u matches the mod's)",
        ns_.c_str(), batch,
        offset ? "shapekey_vertex_offset_original" : "shapekey_dispatch_size_y_original", was, now,
        game_.checksum);
}
