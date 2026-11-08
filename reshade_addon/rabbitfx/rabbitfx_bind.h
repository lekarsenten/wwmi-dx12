#pragma once
// RABBITFX TEXTURE BINDING -- the mod's diffuse/normal/light into an injected draw's PS slots.
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include <reshade.hpp>
#include <d3d12.h>

struct RfxBindState {
    bool bound = false;
    bool applied = false;
    std::vector<std::pair<uint8_t, uint64_t>> tables;
    ID3D12DescriptorHeap* game_srv = nullptr;
    ID3D12DescriptorHeap* game_smp = nullptr;
};
struct WclSlotOvr {
    int reg;
    D3D12_CPU_DESCRIPTOR_HANDLE srv;
    bool is_null;
};
struct ShadowSlotOvr {
    uint8_t param;
    uint32_t offset;
    D3D12_CPU_DESCRIPTOR_HANDLE src;
};
struct RfxDumpBind {
    int reg;
    D3D12_CPU_DESCRIPTOR_HANDLE srv;
    ID3D12Resource* res;
}; // res: the mod texture behind srv, or nullptr

// Binds a draw's overrides through the shadow heap: the game's descriptor tables mirrored with the mod's
// maps in their slots, the extra slots the draw's thread asked for, and the RabbitFX decision (which of the
// mod's maps go to t0..t3, debounced per component and pixel shader until it locks).
class RfxBinder {
public:
    RfxBindState Apply(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl,
        const std::vector<WclSlotOvr>& binds, bool guard_bindless);
    RfxBindState DecideAndBind(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl, int mod_idx,
        int comp_idx, D3D12_CPU_DESCRIPTOR_HANDLE dif, D3D12_CPU_DESCRIPTOR_HANDLE nor, D3D12_CPU_DESCRIPTOR_HANDLE lit,
        D3D12_CPU_DESCRIPTOR_HANDLE mat, bool gbuffer_pass, uint8_t ps_reads,
        const std::vector<WclSlotOvr>* extra = nullptr);
    void Restore(ID3D12GraphicsCommandList* cl, const RfxBindState& st) const;
    bool Param0Covers(
        reshade::api::command_list* cmd_list, unsigned max_reg, unsigned* base_reg, unsigned* count) const;
    void SetExtraSlots(const ShadowSlotOvr* s, unsigned n);
    void AddExtraSlots(const ShadowSlotOvr* s, unsigned n);
    const std::vector<RfxDumpBind>* BindsForDump(unsigned draw_index) const;

private:
    struct Param0Window;
    struct Decision {
        int8_t dec[4] = {};
        bool set = false;
        int8_t cand[4] = {};
        uint16_t cand_cnt = 0;
        uint32_t cand_frame = 0; // the last frame the candidate was counted on
        uint8_t have = 0;
    }; // dec / cand = the four register MASKS d/n/l/m; `have` = which mod maps had resolved (d 1, n 2, l 4, m 8)
    struct DecKey {
        uint64_t mc;
        uint64_t ps;
        bool operator==(const DecKey& o) const { return mc == o.mc && ps == o.ps; }
    };
    struct DecKeyHash {
        size_t operator()(const DecKey& k) const { return (size_t)(k.mc * 0x9E3779B97F4A7C15ull ^ k.ps); }
    };
    static constexpr uint16_t kLockFrames = 8;
    static constexpr uint16_t kRelockFrames = 32;
    static bool ResolveWindow(reshade::api::command_list* cmd_list, Param0Window& w);
    RfxBindState BindThroughShadow(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl,
        const Param0Window& w, const std::vector<WclSlotOvr>& binds);
    struct PerSet;
    std::mutex dec_mutex_;
};
extern RfxBinder g_rfx_bind;
