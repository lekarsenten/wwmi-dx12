// draw_skel_copies.cpp -- see draw_skel_copies.h.
#include "../addon_common.h"
#include "draw_skel_copies.h"
#include "draw_context.h"
#include "draw_toggles.h"
#include "../mods/game_cb_reader.h"
#include "../mods/mod_registry.h"
#include "../mods/port_section.h" // LogBudget
#include "../migoto/port_api.h"
#include <algorithm>
#include <cmath>
#include "../mods/mod_set.h"

SkeletonCopyTracker g_skel_copies;

struct SkeletonCopyTracker::PerSet : SetState {
    std::vector<SkelCopyMod> mods;
    std::unordered_map<uint32_t, CompBoneCb> comp_bone;
};

static SkelBones SkelBonesOf(ID3D12Resource* res, uint64_t va)
{
    float m[kSkelMatchBones * 12] = {};
    const uint32_t got = g_game_cbs.Snapshot(res, va, reinterpret_cast<uint8_t*>(m), sizeof m, true);
    SkelBones b;
    b.n = got / 48;
    for (unsigned i = 0; i < b.n; ++i) {
        b.t[i * 3] = m[i * 12 + 3];
        b.t[i * 3 + 1] = m[i * 12 + 7];
        b.t[i * 3 + 2] = m[i * 12 + 11];
    }
    return b;
}

static double SkelBonesRms(const SkelBones& a, const SkelBones& b)
{
    const unsigned n = a.n < b.n ? a.n : b.n;
    if (!n) return -1.0;
    double d = 0;
    for (unsigned k = 0; k < n * 3; ++k) d += (double)(a.t[k] - b.t[k]) * (a.t[k] - b.t[k]);
    return std::sqrt(d / n);
}

void SkeletonCopyTracker::Roll(SkelCopyMod& m, int dm)
{
    m.prev_slots.clear();
    m.prev_bones.clear();
    size_t comps = 0;
    size_t active = 0;
    for (const SkelCopySeen& e : m.cur) {
        m.prev_slots.push_back(e.slot);
        comps = e.cb4.size() > comps ? e.cb4.size() : comps;
        for (const SkelCopyCb& cb : e.cb4)
            if (cb.va) {
                ++active;
                break;
            }
    }
    size_t most = 0;
    for (size_t c = 0; c < comps; ++c) {
        size_t n = 0;
        for (const SkelCopySeen& e : m.cur)
            if (c < e.cb4.size() && e.cb4[c].va) ++n;
        most = n > most ? n : most;
    }
    m.oversplit = (most && active > most) ? m.oversplit + 1 : 0;
    if (m.oversplit >= kOversplitFrames) {
        m.off = true;
        logger::write(
            "SKCOPIES mod=%d f=%u: for %u frames, %zu vs-cb2 addresses drew components no two of them share -- "
            "vs-cb2 does not tell this character's copies apart; they share one skeleton until the next load",
            dm, m.frame, m.oversplit, active);
    }
    if (m.cur.size() >= 2) {
        unsigned top = 0;
        for (const SkelCopySeen& e : m.cur) top = e.slot > top ? e.slot : top;
        m.prev_bones.assign(top + 1, std::vector<SkelBones>());
        for (const SkelCopySeen& e : m.cur) {
            std::vector<SkelBones>& pb = m.prev_bones[e.slot];
            pb.resize(e.cb4.size());
            for (size_t c = 0; c < e.cb4.size(); ++c)
                if (e.cb4[c].va) pb[c] = e.cb4[c].bones;
        }
    }
    m.prev_keys.clear();
    for (const SkelCopySeen& e : m.cur) {
        m.prev_keys.emplace_back(e.key, e.slot);
        for (const uint64_t k : e.keys) m.prev_keys.emplace_back(k, e.slot);
    }
    m.cur.clear();
}

unsigned SkeletonCopyTracker::CopySlot(reshade::api::command_list* cl, int dm, int comp, ID3D12Resource* b4_res,
    uint64_t b4_va, bool shadow, bool* first_sight)
{
    *first_sight = false;
    if (!g_draw_toggles.skel_copies || dm < 0 || comp < 0) return 0;
    const uint64_t key = ObjectCbOf(cl).va;
    if (!key) return 0;
    std::lock_guard l(mutex_);
    std::vector<SkelCopyMod>& mods = CurrentSet().State<PerSet>().mods;
    if (mods.size() <= (size_t)dm) mods.resize((size_t)dm + 1);
    SkelCopyMod& m = mods[dm];
    if (!m.noted) {
        m.noted = true;
        *first_sight = true;
    }
    if (m.off) return 0;
    if (m.frame != g_frame_id) {
        Roll(m, dm);
        m.frame = g_frame_id;
        if (m.off) return 0;
    }
    size_t at = m.cur.size();
    for (size_t i = 0; i < m.cur.size(); ++i)
        if (m.cur[i].key == key || std::find(m.cur[i].keys.begin(), m.cur[i].keys.end(), key) != m.cur[i].keys.end())
            at = i;
    if (at == m.cur.size() && b4_va)
        for (size_t i = 0; i < m.cur.size(); ++i) {
            SkelCopySeen& o = m.cur[i];
            if (std::find(o.cb4_drawn.begin(), o.cb4_drawn.end(), b4_va) == o.cb4_drawn.end()) continue;
            at = i;
            o.keys.push_back(key);
            if (DiagOnce("SKCOPIES-JOINED", dm))
                logger::write(
                    "SKCOPIES mod=%d comp=%d f=%u: vs-cb2 %llx is drawn with bone CB %llx, already drawn this frame "
                    "under vs-cb2 %llx -- the same copy, on its skeleton #%u",
                    dm, comp, g_frame_id, (unsigned long long)key, (unsigned long long)b4_va, (unsigned long long)o.key,
                    o.slot);
            if (ProbeArmed())
                logger::report(
                    "SKCOPY f=%u mod=%d comp=%d vs-cb2=%llx cb4=%llx%s -> skeleton #%u (its bone CB was drawn under "
                    "vs-cb2 %llx)",
                    g_frame_id, dm, comp, (unsigned long long)key, (unsigned long long)b4_va, shadow ? " shadow" : "",
                    o.slot, (unsigned long long)o.key);
            break;
        }
    auto prev_slot_of = [&](uint64_t k) {
        unsigned s = ~0u;
        for (const auto& pk : m.prev_keys)
            if (pk.first == k) s = pk.second;
        return s;
    };
    const unsigned was = prev_slot_of(key);
    if (at == m.cur.size() && was != ~0u)
        for (size_t i = 0; i < m.cur.size(); ++i)
            if (m.cur[i].slot == was && prev_slot_of(m.cur[i].key) == was) {
                at = i;
                m.cur[i].keys.push_back(key);
                break;
            }
    if (at == m.cur.size()) {
        std::vector<char> taken;
        for (const SkelCopySeen& e : m.cur) {
            if (taken.size() <= e.slot) taken.resize(e.slot + 1, 0);
            taken[e.slot] = 1;
        }
        auto free_slot = [&](unsigned k) { return k >= taken.size() || !taken[k]; };
        unsigned slot = ~0u;
        double best = -1;
        const char* why = "a new copy";
        if (was != ~0u && free_slot(was)) {
            slot = was;
            why = "its skeleton of last frame";
        }
        if (slot == ~0u && !m.prev_bones.empty() && b4_res && b4_va) {
            const SkelBones mine = SkelBonesOf(b4_res, b4_va);
            for (unsigned k : m.prev_slots) {
                if (!free_slot(k) || k >= m.prev_bones.size()) continue;
                const std::vector<SkelBones>& pbs = m.prev_bones[k];
                for (size_t c = 0; c < pbs.size(); ++c) {
                    if (c != (size_t)comp) continue;
                    const double d = SkelBonesRms(mine, pbs[c]);
                    if (d >= 0 && (best < 0 || d < best)) {
                        best = d;
                        slot = k;
                    }
                }
            }
            if (slot != ~0u) why = "nearest bones of last frame";
        }
        if (slot == ~0u)
            for (unsigned k : m.prev_slots)
                if (free_slot(k)) {
                    slot = k;
                    why = "the copy of last frame";
                    break;
                }
        if (slot == ~0u) {
            slot = 0;
            while (!free_slot(slot)) ++slot;
        }
        m.cur.push_back(SkelCopySeen{ key, slot });
        if (was != ~0u && was != slot && g_debug && LogBudget("SKCOPIES-MOVED", Mods().At(dm).wcl_ns, 64))
            logger::write(
                "SKCOPIES-MOVED mod=%d comp=%d f=%u: vs-cb2 %llx drew on skeleton #%u last frame, now on #%u (%s, rms %.3f)",
                dm, comp, g_frame_id, (unsigned long long)key, was, slot, why, best);
        if (was == ~0u && m.prev_keys.size() >= 2 && DiagOnce("SKCOPIES-NEWKEY", dm))
            logger::write(
                "SKCOPIES mod=%d comp=%d f=%u: vs-cb2 %llx was not drawn last frame -- with copies on screen, "
                "its address is new each frame, so only bones tell the copies apart",
                dm, comp, g_frame_id, (unsigned long long)key);
        if (slot && DiagOnce("SKCOPIES-SECOND", dm))
            logger::write(
                "SKCOPIES mod=%d comp=%d f=%u: another copy of this character (vs-cb2 %llx) -- it merges and draws "
                "with its own skeleton #%u",
                dm, comp, g_frame_id, (unsigned long long)key, slot);
        if (ProbeArmed())
            logger::report("SKCOPY f=%u mod=%d comp=%d vs-cb2=%llx cb4=%llx%s -> skeleton #%u (%s, rms %.3f)",
                g_frame_id, dm, comp, (unsigned long long)key, (unsigned long long)b4_va, shadow ? " shadow" : "", slot,
                why, best);
    }
    SkelCopySeen& e = m.cur[at];
    if (b4_va && std::find(e.cb4_drawn.begin(), e.cb4_drawn.end(), b4_va) == e.cb4_drawn.end())
        e.cb4_drawn.push_back(b4_va);
    if (!shadow && b4_va) {
        if (e.cb4.size() <= (size_t)comp) e.cb4.resize((size_t)comp + 1);
        if (!e.cb4[comp].va) {
            e.cb4[comp].va = b4_va;
            e.cb4[comp].bones = SkelBonesOf(b4_res, b4_va);
        } else if (e.cb4[comp].va != b4_va && DiagOnce("SKCOPIES-SHARED", dm)) {
            logger::write(
                "SKCOPIES mod=%d comp=%d f=%u: vs-cb2 %llx is drawn with two bone CBs (%llx, %llx) -- copies under "
                "one per-object address share one skeleton",
                dm, comp, g_frame_id, (unsigned long long)key, (unsigned long long)e.cb4[comp].va,
                (unsigned long long)b4_va);
        }
    }
    return e.slot;
}

void SkeletonCopyTracker::NoteComponentBone(int dm, int comp, uint64_t va)
{
    std::lock_guard<std::mutex> l(comp_bone_mutex_);
    CurrentSet().State<PerSet>().comp_bone[(uint32_t)((dm << 8) | (comp & 0xff))] = { va, g_frame_id };
}

uint64_t SkeletonCopyTracker::ComponentBone(int dm, int comp)
{
    std::lock_guard<std::mutex> l(comp_bone_mutex_);
    const std::unordered_map<uint32_t, CompBoneCb>& comp_bone = CurrentSet().State<PerSet>().comp_bone;
    auto it = comp_bone.find((uint32_t)((dm << 8) | (comp & 0xff)));
    if (it != comp_bone.end() && (uint32_t)(g_frame_id - it->second.frame) <= 2u) return it->second.va;
    return 0;
}
