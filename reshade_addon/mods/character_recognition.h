#pragma once
#include <d3d12.h>
#include <reshade.hpp>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "mod_registry.h"
#include "component_table.h"
#include "../instrumentation/diag_gate.h"

struct CharVbOrigin {
    uint32_t frame = 0;
    uint64_t ns_hash = 0;
};
struct SkinOut {
    uint64_t va;
    uint32_t size;
};

// Hash the first N bone matrices of an UPLOAD bone CB (the DX12 equivalent of WWMI's MarkBoneDataCB hash).
uint32_t HashBoneCB(ID3D12Resource* res, uint64_t va);

// What has been learned about the characters on screen: the mesh buffers a claimed draw bound, the skeletons of
// the frame, the LOD meshes those skeletons link, the index buffers a mod's components anchor to, the skin-cache
// dispatches and their outputs, and the address band the engine skins into.
class CharacterRecognition {
public:
    bool CharMeshOrigin(uint64_t vb0, CharVbOrigin* out);
    bool IsCharMeshVb0(uint64_t vb0);
    void NoteCharMeshVb0(uint64_t vb0, const CharVbOrigin& origin);
    void ForgetCharMeshVb0(uint64_t va, uint64_t bytes);
    bool AnyCharMesh();
    size_t CharMeshCount();
    // f(va, const CharVbOrigin&) under the lock; true from f stops the walk.
    template <class F> void ForEachCharMeshVb0(F f)
    {
        std::lock_guard l(char_mesh_mutex_);
        for (const auto& kv : char_mesh_vb0_)
            if (f(kv.first, kv.second)) break;
    }

    // The frame's character skeleton CONTENT hashes, and the LOD mesh buffers drawn under one (persistent).
    bool InFrameSkel(uint32_t hash);
    void NoteFrameSkel(uint32_t hash);
    bool IsCharLodVb0(uint64_t vb0);
    bool RecogniseLod(uint32_t skel_hash, uint64_t vb0, bool* linked_now);

    // INDEX-BUFFER ANCHOR: an index buffer carrying a mod's complete declared component set is that mod's.
    bool IbAnchorNoteAndAsk(uint64_t ib_va, uint64_t ib_bytes, int mod_id, int comp_idx); // ib_bytes: the IB's extent
    void IbAnchorForget(uint64_t va, uint64_t bytes);
    bool IbAnchorResolve(uint64_t index_va, uint32_t index_bytes, int* mod_id, uint64_t* ib_base_va);
    bool IbAnchorJustLearned(uint64_t index_va); // this is the first frame its draws are claimed

    // Texture-identity ownership: the draw binds a texture a mod's rules target.
    bool DrawBindsModTexture(reshade::api::command_list* cmd_list);
    // IB-HASH SUPPRESS: ini `hash=` values of components that HAVE claimed via (first,count) this session.
    bool IbHashEverClaimed(uint32_t ib_hash);
    void NoteClaimedIbHash(uint32_t ib_hash);
    // SKIPONLY: the mesh hashes whose game draws a mod drops; no lock on the draw path unless a mod declares one.
    bool SkipVbHashAny() const { return skip_vbhash_any_.load(std::memory_order_relaxed); }
    std::shared_ptr<const std::unordered_set<uint32_t>> SkipVbHashes();
    void SetSkipVbHashes(std::shared_ptr<const std::unordered_set<uint32_t>> hashes);
    void NoteSkipVbHashDrop() { ++skip_vbhash_drops_; }
    uint32_t SkipVbHashDrops() const { return skip_vbhash_drops_.load(); }
    // SMALLMESH: mesh hashes of sections that only set variables, test them or skip (RabbitFX's menu flag), run by a
    // small unskinned draw; Gen() moves with each new set, so a reader re-takes it only then.
    bool SmallMeshAny() const { return small_mesh_any_.load(std::memory_order_relaxed); }
    uint32_t SmallMeshGen() const { return small_mesh_gen_.load(std::memory_order_acquire); }
    std::shared_ptr<const std::unordered_set<uint32_t>> SmallMeshHashes(uint32_t* gen); // gen: the set's, read with it
    void SetSmallMeshHashes(std::shared_ptr<const std::unordered_set<uint32_t>> hashes);
    // MODELFLAGS: the model hashes other mods' variable-only sections are keyed to. An unclaimed draw of one is
    // noted for the present; a claimed one tells through its mod's claim age.
    bool ModelHashAny() const { return model_any_.load(std::memory_order_relaxed); }
    void SetModelHashes(std::shared_ptr<const std::unordered_set<uint32_t>> hashes);
    void NoteModelDrawn(uint32_t hash);
    void ModelsDrawnWithin(uint32_t frames, std::unordered_set<uint32_t>& out);

    // The engine's skinned-vertex VA pool band, anchored on the learned VB0s every present.
    bool InSkinBand(uint64_t va) const { return va >= skin_band_lo_.load() && va < skin_band_hi_.load(); }
    uint64_t SkinBandLo() const { return skin_band_lo_.load(); }
    uint64_t SkinBandHi() const { return skin_band_hi_.load(); }
    void UpdateSkinBand();

    // Skin-cache lineage: the compute shaders seen skinning our character, and the pool ranges they write.
    bool SkinCsAccepts(uint64_t cs_hash); // none known yet, or this one
    bool IsSkinCs(uint64_t cs_hash);
    // Notes the outputs (duplicates by address dropped) and the shader; true the first time for this shader.
    bool LearnSkinCs(uint64_t cs_hash, const std::vector<SkinOut>& outs, int* added, size_t* total);
    bool InSkinOut(uint64_t vb0);
    size_t SkinOutCount();
    void ForgetSkinOutRange(uint64_t va, uint64_t bytes);
    // The frame's character bone-CB VAs (POOLDRAW boneNear), at most 64.
    void NoteCharBoneVa(uint64_t cur_va, uint64_t prev_va);
    uint64_t CharBoneDistance(uint64_t va); // ~0ull when none noted
    // Opens the per-frame scopes: the skeleton set (LOD-pin) and the bone-CB VAs (POOLDRAW boneNear).
    void BeginFrame();

    // SEEDHB heartbeat aggregation: the VB0 range of substantial draws this second (draw tallies, present flushes).
    void NoteHeartbeatDraw(uint64_t vb0);
    void TakeHeartbeat(uint64_t* vbmin, uint64_t* vbmax, uint32_t* large, uint32_t* pool);

private:
    struct CompSet {
        std::vector<uint64_t> w;
        void set(int i);
        bool empty() const;
        bool covers(const CompSet& d) const;
        unsigned count() const;
    };
    struct IbEvidence {
        uint32_t frame = 0;
        uint64_t ns_hash = 0;
        CompSet seen;
    };
    struct IbAnchorRec {
        uint64_t ns_hash = 0;
        uint32_t learned = 0;
        uint32_t confirmed = 0;
        uint64_t bytes = 0;
    };
    static constexpr size_t kIbAnchorCap = 256; // a character is one entry; a sweep past this is a leak guard
    static constexpr uint32_t kIbAnchorStale = 1800; // ~30 s unconfirmed -- the buffer is not on screen any more
    struct PerSet;
    CompSet ModDeclaredComps(int mod_id);

    std::mutex char_mesh_mutex_;
    std::unordered_map<uint64_t, CharVbOrigin> char_mesh_vb0_;
    std::mutex char_skel_mutex_; // frame_skel_ and char_lod_vb0_
    std::unordered_set<uint32_t> frame_skel_;
    std::unordered_set<uint64_t> char_lod_vb0_;
    std::mutex ib_anchor_mutex_; // ib_ev_ and ib_anchor_
    std::map<uint64_t, IbEvidence> ib_ev_; // ordered, so IbAnchorForget can erase an address range
    std::map<uint64_t, IbAnchorRec> ib_anchor_;
    std::mutex claimed_ibhash_mutex_;
    std::unordered_set<uint32_t> claimed_ibhash_;
    std::mutex skip_vbhash_mutex_;
    std::shared_ptr<const std::unordered_set<uint32_t>> skip_vbhash_;
    std::atomic<bool> skip_vbhash_any_{ false };
    std::atomic<uint32_t> skip_vbhash_drops_{ 0 };
    std::mutex small_mesh_mutex_;
    std::shared_ptr<const std::unordered_set<uint32_t>> small_mesh_;
    std::atomic<bool> small_mesh_any_{ false };
    std::atomic<uint32_t> small_mesh_gen_{ 0 }; // bumped with each new set, under small_mesh_mutex_
    std::mutex model_mutex_;
    std::shared_ptr<const std::unordered_set<uint32_t>> model_hashes_;
    std::unordered_map<uint32_t, uint32_t> model_drawn_; // hash -> the frame an unclaimed draw of it was seen
    std::atomic<bool> model_any_{ false };
    std::atomic<uint32_t> model_gen_{ 0 }; // bumped with each new set, under model_mutex_
    std::atomic<uint64_t> skin_band_lo_{ 0x1000000000ULL };
    std::atomic<uint64_t> skin_band_hi_{ 0x1100000000ULL };
    std::mutex skincache_mutex_; // char_skin_cs_, skincache_out_ and char_bone_va_
    std::unordered_set<uint64_t> char_skin_cs_;
    std::vector<SkinOut> skincache_out_;
    std::unordered_set<uint64_t> char_bone_va_;
    std::mutex hb_mutex_;
    uint64_t hb_vbmin_ = ~0ull;
    uint64_t hb_vbmax_ = 0;
    uint32_t hb_large_ = 0;
    uint32_t hb_pool_ = 0;
};
extern CharacterRecognition g_recognition;
