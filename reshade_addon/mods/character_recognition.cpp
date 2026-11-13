// character_recognition.cpp -- see character_recognition.h.
#include "../addon_common.h"
#include "character_recognition.h"
#include "component_table.h"
#include "game_cb_reader.h"
#include "layout_registry.h"
#include "../core/fnv.h"
#include "../draw/draw_toggles.h"
#include "../instrumentation/diag_gate.h"
#include "../hooks/command_list_registry.h"
#include "../instrumentation/profiler.h"
#include "../textures/tex_rules.h"
#include "../textures/tex_select.h"
#include "../textures/tex_lod.h"
#include "mod_set.h"

CharacterRecognition g_recognition;

bool CharacterRecognition::CharMeshOrigin(uint64_t vb0, CharVbOrigin* out)
{
    std::lock_guard l(char_mesh_mutex_);
    auto cv = char_mesh_vb0_.find(vb0);
    if (cv == char_mesh_vb0_.end()) return false;
    *out = cv->second;
    return true;
}

bool CharacterRecognition::IsCharMeshVb0(uint64_t vb0)
{
    std::lock_guard l(char_mesh_mutex_);
    return char_mesh_vb0_.count(vb0) != 0;
}

void CharacterRecognition::NoteCharMeshVb0(uint64_t vb0, const CharVbOrigin& origin)
{
    std::lock_guard l(char_mesh_mutex_);
    char_mesh_vb0_[vb0] = origin;
}

void CharacterRecognition::ForgetCharMeshVb0(uint64_t va, uint64_t bytes)
{
    if (!va) return;
    if (!bytes) bytes = 1;
    unsigned n = 0;
    {
        std::lock_guard l(char_mesh_mutex_);
        for (auto it = char_mesh_vb0_.begin(); it != char_mesh_vb0_.end();)
            if (it->first >= va && it->first - va < bytes) {
                it = char_mesh_vb0_.erase(it);
                ++n;
            } else
                ++it;
    }
    if (n && logger::enabled(2))
        logger::write("CHARVB: forgot %u learned VB0(s) in destroyed buffer va=%llx bytes=%llu", n,
            (unsigned long long)va, (unsigned long long)bytes);
}

bool CharacterRecognition::AnyCharMesh()
{
    std::lock_guard l(char_mesh_mutex_);
    return !char_mesh_vb0_.empty();
}

size_t CharacterRecognition::CharMeshCount()
{
    std::lock_guard l(char_mesh_mutex_);
    return char_mesh_vb0_.size();
}

bool CharacterRecognition::InFrameSkel(uint32_t hash)
{
    std::lock_guard l(char_skel_mutex_);
    return frame_skel_.count(hash) != 0;
}

void CharacterRecognition::NoteFrameSkel(uint32_t hash)
{
    std::lock_guard l(char_skel_mutex_);
    frame_skel_.insert(hash);
}

bool CharacterRecognition::IsCharLodVb0(uint64_t vb0)
{
    std::lock_guard l(char_skel_mutex_);
    return char_lod_vb0_.count(vb0) != 0;
}

bool CharacterRecognition::RecogniseLod(uint32_t skel_hash, uint64_t vb0, bool* linked_now)
{
    std::lock_guard l(char_skel_mutex_);
    if (skel_hash && vb0 && frame_skel_.count(skel_hash)) {
        if (char_lod_vb0_.insert(vb0).second) *linked_now = true;
    }
    return vb0 && char_lod_vb0_.count(vb0) != 0;
}

void CharacterRecognition::CompSet::set(int i)
{
    const size_t k = (size_t)i / 64;
    if (w.size() <= k) w.resize(k + 1, 0);
    w[k] |= 1ull << (i % 64);
}

bool CharacterRecognition::CompSet::empty() const
{
    for (uint64_t x : w)
        if (x) return false;
    return true;
}

bool CharacterRecognition::CompSet::covers(const CompSet& d) const
{
    for (size_t k = 0; k < d.w.size(); ++k)
        if ((d.w[k] & (k < w.size() ? w[k] : 0)) != d.w[k]) return false;
    return true;
}

unsigned CharacterRecognition::CompSet::count() const
{
    unsigned n = 0;
    for (uint64_t x : w)
        while (x) {
            x &= x - 1;
            ++n;
        }
    return n;
}

struct CharacterRecognition::PerSet : SetState {
    std::unordered_map<int, CompSet> declared;
    void Build(ModSet& set) override
    {
        for (const MergedComp& c : set.comps.All())
            if (c.comp_idx >= 0) declared[c.mod_id].set(c.comp_idx);
    }
};

CharacterRecognition::CompSet CharacterRecognition::ModDeclaredComps(int mod_id)
{
    const std::unordered_map<int, CompSet>& declared = CurrentSet().State<PerSet>().declared;
    auto it = declared.find(mod_id);
    return it == declared.end() ? CompSet{} : it->second;
}

bool CharacterRecognition::IbAnchorNoteAndAsk(
    uint64_t ib_va, uint64_t ib_bytes, int mod_id, int comp_idx, bool content_proven)
{
    if (!ib_va || mod_id < 0 || comp_idx < 0 || mod_id >= (int)Mods().Count()) return false;
    const CompSet declared = ModDeclaredComps(mod_id);
    if (declared.empty()) return false;
    const uint64_t ns_hash = Mods().At(mod_id).ns_hash;
    if (!ns_hash) return false;
    std::lock_guard l(ib_anchor_mutex_);
    IbEvidence& e = ib_ev_[ib_va];
    if (e.frame != g_frame_id || e.ns_hash != ns_hash) {
        e.frame = g_frame_id;
        e.ns_hash = ns_hash;
        e.seen = CompSet{};
    }
    e.seen.set(comp_idx);
    const bool shape_only = declared.count() < 2 && !content_proven;
#ifdef WWMI_DEBUG
    if (shape_only && e.seen.covers(declared) && DiagOnce("IBANCHOR-SHAPE", (int)(uint32_t)(ns_hash ^ (ns_hash >> 32))))
        logger::write("IBANCHOR-SHAPE mod %d: index buffer %llx holds a draw the shape of its only component, but "
                      "nothing in it proves it is the mod's mesh -- not anchored",
            mod_id, (unsigned long long)ib_va);
#endif
    if (!shape_only && e.seen.covers(declared)) {
        IbAnchorRec& a = ib_anchor_[ib_va];
        a.bytes = ib_bytes;
        if (a.ns_hash != ns_hash) {
            a.ns_hash = ns_hash;
            a.learned = g_frame_id;
            g_draw_stats.ib_anchor_learned.fetch_add(1, std::memory_order_relaxed);
            logger::write("IBANCHOR learned: index buffer %llx carries mod %d's COMPLETE declared component "
                          "set (%u components) in frame %u -- its draws are claimed from the next frame on, "
                          "with no vertex content consulted",
                (unsigned long long)ib_va, mod_id, declared.count(), g_frame_id);
        }
        a.confirmed = g_frame_id;
    }
    if (ib_anchor_.size() > kIbAnchorCap)
        for (auto it = ib_anchor_.begin(); it != ib_anchor_.end();)
            if (g_frame_id - it->second.confirmed > kIbAnchorStale)
                it = ib_anchor_.erase(it);
            else
                ++it;
    auto it = ib_anchor_.find(ib_va);
    if (it == ib_anchor_.end() || it->second.ns_hash != ns_hash) return false;
    return it->second.learned < g_frame_id;
}

bool CharacterRecognition::IbAnchorResolve(uint64_t index_va, uint32_t index_bytes, int* mod_id, uint64_t* ib_base_va)
{
    if (!index_va || !index_bytes) return false;
    std::lock_guard l(ib_anchor_mutex_);
    auto it = ib_anchor_.upper_bound(index_va);
    if (it == ib_anchor_.begin()) return false;
    --it; // the greatest anchored base at or below the address
    const uint64_t off = index_va - it->first;
    if (!it->second.bytes || off >= it->second.bytes || (off % index_bytes) != 0 || !it->second.ns_hash) return false;
    *mod_id = Mods().IdOfNsHash(it->second.ns_hash);
    if (*mod_id < 0) return false;
    *ib_base_va = it->first;
    return true;
}

bool CharacterRecognition::IbAnchorJustLearned(uint64_t index_va)
{
    if (!index_va) return false;
    std::lock_guard l(ib_anchor_mutex_);
    auto it = ib_anchor_.upper_bound(index_va);
    if (it == ib_anchor_.begin()) return false;
    --it;
    return index_va - it->first < it->second.bytes && it->second.learned + 1 == g_frame_id;
}

void CharacterRecognition::IbAnchorForget(uint64_t va, uint64_t bytes)
{
    if (!va) return;
    if (!bytes) bytes = 1;
    std::lock_guard l(ib_anchor_mutex_);
    ib_ev_.erase(ib_ev_.lower_bound(va), ib_ev_.lower_bound(va + bytes));
    ib_anchor_.erase(ib_anchor_.lower_bound(va), ib_anchor_.lower_bound(va + bytes));
}

uint32_t HashBoneCB(ID3D12Resource* res, uint64_t va)
{
    PROF(PROF_DI_BoneHash);
    uint8_t buf[1536];
    const uint32_t n = g_game_cbs.Snapshot(res, va, buf, sizeof(buf));
    if (!n) return 0;
    const uint32_t h = Fnv1a32(buf, n);
    return h ? h : 1;
}

static void CollectBoundModTexHashes(command_list* cmd_list, uint32_t* out, int cap, int& n)
{
    n = 0;
    const TexRules& tr = g_tex_rules.Now();
    if (tr.targets.empty()) return;
    PROF(PROF_DI_TexScan); // the per-draw table walk behind char_tex
    const auto& root = g_command_lists.Gfx(cmd_list).root;
    uint32_t scan_len[64];
    g_layouts.TexScanLens(g_command_lists.GfxLayout(cmd_list), scan_len);
    for (int p = 0; p < 64; ++p) {
        const TableStart ts = TableStartOf(cmd_list->get_device(), root[p].table);
        if (!ts) continue;
        uint32_t num = ts.Heap()->GetDesc().NumDescriptors;
        ID3D12Resource* res[kTexScanMax];
        uint32_t hs[kTexScanMax];
        const uint32_t maxS = g_tex_select.ResolveBound(ts.heap, ts.base, num, res, hs, scan_len[p]);
        for (uint32_t s = 0; s < maxS; ++s) {
            const uint32_t h = !hs[s] || tr.targets.count(hs[s]) ? hs[s] : g_tex_select.LodTargetOf(hs[s]);
            if (h && tr.targets.count(h)) {
                if (n < cap) out[n++] = h;
                if (n >= cap) return;
            }
        }
    }
}

bool CharacterRecognition::DrawBindsModTexture(command_list* cmd_list)
{
    uint32_t hs[8];
    int n = 0;
    CollectBoundModTexHashes(cmd_list, hs, 8, n);
    return n > 0;
}

bool CharacterRecognition::IbHashEverClaimed(uint32_t ib_hash)
{
    std::lock_guard l(claimed_ibhash_mutex_);
    return claimed_ibhash_.count(ib_hash) != 0;
}

void CharacterRecognition::NoteClaimedIbHash(uint32_t ib_hash)
{
    std::lock_guard l(claimed_ibhash_mutex_);
    claimed_ibhash_.insert(ib_hash);
}

std::shared_ptr<const std::unordered_set<uint32_t>> CharacterRecognition::SkipVbHashes()
{
    std::lock_guard l(skip_vbhash_mutex_);
    return skip_vbhash_;
}

void CharacterRecognition::SetSkipVbHashes(std::shared_ptr<const std::unordered_set<uint32_t>> hashes)
{
    std::lock_guard l(skip_vbhash_mutex_);
    skip_vbhash_ = std::move(hashes);
    skip_vbhash_any_ = skip_vbhash_ != nullptr;
}

std::shared_ptr<const std::unordered_set<uint32_t>> CharacterRecognition::SmallMeshHashes(uint32_t* gen)
{
    std::lock_guard l(small_mesh_mutex_);
    *gen = small_mesh_gen_.load(std::memory_order_relaxed);
    return small_mesh_;
}

void CharacterRecognition::SetSmallMeshHashes(std::shared_ptr<const std::unordered_set<uint32_t>> hashes)
{
    std::lock_guard l(small_mesh_mutex_);
    small_mesh_ = std::move(hashes);
    small_mesh_any_ = small_mesh_ != nullptr;
    small_mesh_gen_.fetch_add(1);
}

void CharacterRecognition::SetModelHashes(std::shared_ptr<const std::unordered_set<uint32_t>> hashes)
{
    std::lock_guard l(model_mutex_);
    model_hashes_ = std::move(hashes);
    model_drawn_.clear();
    model_any_ = model_hashes_ != nullptr;
    model_gen_.fetch_add(1);
}

void CharacterRecognition::NoteModelDrawn(uint32_t hash)
{
    thread_local uint32_t t_gen = ~0u;
    thread_local std::shared_ptr<const std::unordered_set<uint32_t>> t_hashes;
    if (model_gen_.load(std::memory_order_acquire) != t_gen) {
        std::lock_guard l(model_mutex_);
        t_hashes = model_hashes_;
        t_gen = model_gen_.load(std::memory_order_relaxed);
    }
    if (!t_hashes || !t_hashes->count(hash)) return;
    std::lock_guard l(model_mutex_);
    model_drawn_[hash] = g_frame_id;
}

void CharacterRecognition::ModelsDrawnWithin(uint32_t frames, std::unordered_set<uint32_t>& out)
{
    std::lock_guard l(model_mutex_);
    for (const auto& [hash, frame] : model_drawn_)
        if (g_frame_id - frame <= frames) out.insert(hash);
}

void CharacterRecognition::UpdateSkinBand()
{
    uint64_t mn = ~0ull;
    uint64_t mx = 0;
    {
        std::lock_guard l(char_mesh_mutex_);
        for (const auto& kv : char_mesh_vb0_) {
            const uint64_t v = kv.first;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    }
    if (mn == ~0ull) return; // nothing learned yet -> keep fallback
    skin_band_lo_.store(mn & ~0xFFFFFFFFull); // round min down to 4GB boundary
    skin_band_hi_.store((mx & ~0xFFFFFFFFull) + 0x200000000ull); // cover max + ~8GB pool headroom
}

bool CharacterRecognition::SkinCsAccepts(uint64_t cs_hash)
{
    std::lock_guard l(skincache_mutex_);
    return char_skin_cs_.empty() || char_skin_cs_.count(cs_hash);
}

bool CharacterRecognition::IsSkinCs(uint64_t cs_hash)
{
    std::lock_guard l(skincache_mutex_);
    return char_skin_cs_.count(cs_hash) != 0;
}

bool CharacterRecognition::LearnSkinCs(uint64_t cs_hash, const std::vector<SkinOut>& outs, int* added, size_t* total)
{
    std::lock_guard l(skincache_mutex_);
    *added = 0;
    for (const SkinOut& b : outs) {
        bool dup = false;
        for (auto& o : skincache_out_)
            if (o.va == b.va) {
                dup = true;
                break;
            }
        if (!dup) {
            skincache_out_.push_back({ b.va, b.size });
            ++*added;
        }
    }
    *total = skincache_out_.size();
    return char_skin_cs_.insert(cs_hash).second;
}

bool CharacterRecognition::InSkinOut(uint64_t vb0)
{
    std::lock_guard l(skincache_mutex_);
    for (auto& o : skincache_out_)
        if (vb0 >= o.va && vb0 < o.va + o.size) return true;
    return false;
}

size_t CharacterRecognition::SkinOutCount()
{
    std::lock_guard l(skincache_mutex_);
    return skincache_out_.size();
}

void CharacterRecognition::ForgetSkinOutRange(uint64_t va, uint64_t bytes)
{
    std::lock_guard l(skincache_mutex_);
    std::erase_if(skincache_out_, [&](const SkinOut& o) { return o.va >= va && o.va < va + bytes; });
}

void CharacterRecognition::NoteCharBoneVa(uint64_t cur_va, uint64_t prev_va)
{
    std::lock_guard l(skincache_mutex_);
    if (cur_va && char_bone_va_.size() < 64) char_bone_va_.insert(cur_va);
    if (prev_va && char_bone_va_.size() < 64) char_bone_va_.insert(prev_va);
}

uint64_t CharacterRecognition::CharBoneDistance(uint64_t va)
{
    uint64_t bdist = ~0ull;
    std::lock_guard l(skincache_mutex_);
    for (uint64_t v : char_bone_va_) {
        uint64_t d = va > v ? va - v : v - va;
        if (d < bdist) bdist = d;
    }
    return bdist;
}

void CharacterRecognition::BeginFrame()
{
    {
        std::lock_guard l(char_skel_mutex_);
        frame_skel_.clear();
    }
    {
        std::lock_guard l(skincache_mutex_);
        char_bone_va_.clear();
    }
}

void CharacterRecognition::NoteHeartbeatDraw(uint64_t vb0)
{
    std::lock_guard l(hb_mutex_);
    ++hb_large_;
    if (vb0 < hb_vbmin_) hb_vbmin_ = vb0;
    if (vb0 > hb_vbmax_) hb_vbmax_ = vb0;
    if (InSkinBand(vb0)) ++hb_pool_;
}

void CharacterRecognition::TakeHeartbeat(uint64_t* vbmin, uint64_t* vbmax, uint32_t* large, uint32_t* pool)
{
    std::lock_guard l(hb_mutex_);
    *vbmin = hb_vbmin_;
    *vbmax = hb_vbmax_;
    *large = hb_large_;
    *pool = hb_pool_;
    hb_vbmin_ = ~0ull;
    hb_vbmax_ = 0;
    hb_large_ = 0;
    hb_pool_ = 0;
}
