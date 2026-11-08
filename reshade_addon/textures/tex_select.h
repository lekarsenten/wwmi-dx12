#pragma once
#include <d3d12.h>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <string>

// Which mod texture a game texture gets: the rule its hash (or its proven low's target) names, and the selection
// that rule's section makes under the mod's current variables.
class TexSelector {
public:
    // Resolve the selected mod TexResource index for a vanilla hash (honors $var/key conditionals). -1 = none.
    int SelectIdx(uint32_t hash, unsigned mip);
    int SelectIdxNs(uint32_t hash, const wchar_t* ns, uint64_t ns_hash, unsigned mip);
    // How many mips down the game draws `hash` against the mod target `target` (A20): 0 for the target itself, else
    // its proven low's mip; unknown otherwise.
    unsigned DrawnMip(uint32_t hash, uint32_t target);
    uint32_t RuleHashFor(uint32_t hash);
    // The rule the texture's own data proves -- its hash, or a texture-cache link -- never a pipe alias: what the
    // texture cache learns is taken from this.
    uint32_t ProvenRuleFor(uint32_t hash);
    // The mod target `hash` is a lower mip or (A13) a look-alike of, from the texture cache, else 0.
    uint32_t LodTargetOf(uint32_t hash);
    uint32_t ResolveBound(uint64_t heap, uint32_t base, uint32_t num, ID3D12Resource** res, uint32_t* hash,
        uint32_t cap, const std::bitset<512>* want = nullptr);
    // Resolve a matched texture hash to a mod SRV via its rule's conditional assigns (last match wins).
    D3D12_CPU_DESCRIPTOR_HANDLE Replacement(uint32_t hash);
    // Resolve a fully-qualified mod resource name ("ns\resourcename") -> its loaded mod-texture SRV ({0} if unknown).
    D3D12_CPU_DESCRIPTOR_HANDLE ModSrvByResourceFq(const std::wstring& res_fq);
    D3D12_CPU_DESCRIPTOR_HANDLE RfxMapSrv(const std::wstring& res_fq, const wchar_t* ns);
    void ArmFlickerWatch(int frames) { flicker_watch_.store(frames); } // the overlay (B4)
    void TickFlickerWatch(); // once per present

private:
    static int WclSelect(uint32_t hash);
    static int Touched(int sel, unsigned mip);
    void FlickerWatch(uint32_t hash, int sel);

    std::atomic<int> flicker_watch_{ 0 }; // frames remaining (TEXFLICKER)
};
extern TexSelector g_tex_select;
