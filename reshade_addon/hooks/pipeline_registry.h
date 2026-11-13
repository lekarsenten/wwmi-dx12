#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <reshade.hpp>
#include "dxil_psv.h"

// What the two creation hooks cost since the last take: calls and the time spent in them, all threads summed.
struct PipelineHookStats {
    uint64_t created;
    uint64_t created_us;
    uint64_t created_max_us;
    uint64_t destroyed;
    uint64_t destroyed_us;
};

// Every PSO the game made, by handle: its shader hashes, what its shaders read, its cull mode, and the variants
// of it the add-on builds (decensor, UID cut). The handle table is lock-free for the bind hook's path.
class PipelineRegistry {
public:
    // The shader hashes of a PSO (vs or cs, ps, gs; 0 = none), read without a lock -- the bind hook's path.
    void BoundHashes(uint64_t pso, uint64_t* vs_or_cs, uint64_t* ps, uint64_t* gs);
    // The PSO's decensor-only variant, lock-free; 0 = none (yet), or its pixel shader has not drawn a character.
    // The first ask queues the build.
    uint64_t DecensorVariant(reshade::api::device* dev, uint64_t pso);
    void SetDecensorVariant(uint64_t pso, uint64_t variant);
    // A character draw (a mod's, or any skinned one) runs on `pso`: its pixel shader is one DecensorVariant answers
    // for, from now on.
    void NoteCharacterDraw(uint64_t pso);
    // Its input layout reads vertex stream 6, a character mesh's; lock-free.
    bool ReadsStream6(uint64_t pso);
    // The PSO's UID-cut variant for a back buffer of `target` (UidHide::Target), lock-free; 0 = none (yet), or the
    // PSO does not draw UI text. The first ask for a target queues the build.
    uint64_t UidVariant(reshade::api::device* dev, uint64_t pso, uint32_t target);
    void SetUidVariant(uint64_t pso, uint32_t target, uint64_t variant);
    // The ps-tN its pixel shader may read, bit r for t0..t63 (DxilSrvRegs::reads), lock-free; all set when unknown.
    uint64_t PsReadMask(uint64_t pso);
    // A copy of the set for `pso` / stage ('p' or 'v'), under the guard; an unparsed default when unknown.
    DxilSrvRegs SrvRegs(uint64_t pso, char stage);
    bool VsCbufferBytes(uint64_t pso, unsigned reg, uint32_t& bytes);
    bool PsCbufferBytes(uint64_t pso, unsigned reg, uint32_t& bytes);
    // All of a stage's ('v' or 'p') at once: b0..b15, 0 = not declared.
    bool CbufferBytes(uint64_t pso, char stage, std::array<uint32_t, 16>& bytes);
    // The PSO's rasterizer cull mode, recorded at creation for every PSO; false when it stated none.
    bool CullMode(uint64_t pso, reshade::api::cull_mode* out);
    PipelineHookStats TakeStats();

    // The creation hooks' work: the PSO's shaders registered and read, its record made; and unmade.
    void Register(reshade::api::device* device, reshade::api::pipeline_layout layout, uint32_t subobject_count,
        const reshade::api::pipeline_subobject* subobjects, reshade::api::pipeline pipeline);
    void Forget(reshade::api::device* device, reshade::api::pipeline pipeline);

private:
    struct Slot {
        std::atomic<uint64_t> key{ 0 };
        std::atomic<uint64_t> vc{ 0 };
        std::atomic<uint64_t> ps{ 0 };
        std::atomic<uint64_t> gs{ 0 };
        std::atomic<uint64_t> dc{ 0 }; // 0 = not asked, 1 = asked / none, else the decensor variant
        std::atomic<uint64_t> psr{ ~0ull }; // PsReadMask
        std::atomic<bool> text{ false }; // its PS draws UI text
        std::atomic<uint64_t> uc{ 0 }; // as dc, the UID-cut variant for the back buffer size ucz
        std::atomic<uint32_t> ucz{ 0 };
        std::atomic<bool> chr{ false }; // its PS was seen on a character draw, as of character-shader generation chrg
        std::atomic<uint32_t> chrg{ 0 };
        std::atomic<bool> vb6{ false }; // its input layout reads vertex stream 6
    };
    struct Tab {
        uint32_t cap;
        Slot* e;
    };
    static uint32_t Hash(uint64_t key, uint32_t cap);
    static Slot* FindIn(const Tab* t, uint64_t key);
    static Slot* PutIn(Tab* t, uint64_t key);
    Slot* SlotOf(uint64_t key, bool create);
    bool CharacterPs(Slot* s, uint64_t ps);
    void MarkCharacterPs(Slot* s, const char* why);

    std::atomic<Tab*> tab_{ new Tab{ 1u << 16, new Slot[1u << 16] } };
    std::mutex add_;
    uint32_t used_ = 0; // under add_
    std::mutex char_ps_mutex_;
    std::unordered_set<uint64_t> char_ps_; // under char_ps_mutex_
    std::atomic<uint32_t> char_ps_gen_{ 0 };
    std::mutex regs_mutex_; // guards the five maps below
    std::unordered_map<uint64_t, DxilSrvRegs> ps_regs_;
    std::unordered_map<uint64_t, DxilSrvRegs> vs_regs_;
    std::unordered_map<uint64_t, std::array<uint32_t, 16>> vs_cbs_; // b0..b15 (space 0), 0 = not declared
    std::unordered_map<uint64_t, std::array<uint32_t, 16>> ps_cbs_; // the same for the pixel shader
    std::unordered_map<uint64_t, reshade::api::cull_mode> cull_;
    std::atomic<uint64_t> init_n_{ 0 };
    std::atomic<uint64_t> init_us_{ 0 };
    std::atomic<uint64_t> init_max_us_{ 0 };
    std::atomic<uint64_t> destroy_n_{ 0 };
    std::atomic<uint64_t> destroy_us_{ 0 };
};
extern PipelineRegistry g_pipelines;

bool on_create_pipeline(reshade::api::device*, reshade::api::pipeline_layout, uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects);
void on_init_pipeline(reshade::api::device* device, reshade::api::pipeline_layout layout, uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects, reshade::api::pipeline pipeline);
void on_destroy_pipeline(reshade::api::device* device, reshade::api::pipeline pipeline);
