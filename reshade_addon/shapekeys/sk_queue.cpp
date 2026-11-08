// The dispatch-scope queue: the lists a game dispatch's `run =` queued, tagged with that dispatch.
#include "sk_internal.h"
#include "../mods/mod_set.h"

ShapekeyQueue g_sk_queue;

static thread_local unsigned long long t_sk_cur_cs = 0;
static thread_local unsigned t_sk_cur_tgy = 0;
static thread_local unsigned t_sk_cur_u0hash = 0;
static thread_local int t_sk_cur_owner = -1;
static thread_local uint64_t t_sk_cur_owner_hash = 0;
static thread_local ID3D12Resource* t_sk_cur_inst = nullptr;
static thread_local unsigned t_q_count = 0;
static thread_local uint8_t t_q_reason = 0;

void ShapekeyQueue::NoteDispatchList(const void* native_cl, uint32_t frame)
{
    DispAnchor& a = disp_[frame & 1];
    a.cl.store(native_cl, std::memory_order_relaxed);
    a.frame.store(frame, std::memory_order_relaxed);
}

const void* ShapekeyQueue::DispListForFrame(uint32_t f) const
{
    const DispAnchor& a = disp_[f & 1];
    return (a.frame.load(std::memory_order_relaxed) == f) ? a.cl.load(std::memory_order_relaxed) : nullptr;
}

unsigned ShapekeyQueue::TakeOutcome(uint8_t* reason)
{
    if (reason) *reason = t_q_reason;
    const unsigned n = t_q_count;
    t_q_count = 0;
    t_q_reason = 0;
    return n;
}

void ShapekeyQueue::BeginDispatchScope(uint64_t cs_hash, uint32_t group_count_y, const SkU0Ident& ck)
{
    t_q_count = 0;
    t_q_reason = ck.shapekey ? 3u : 1u;
    t_sk_cur_cs = (unsigned long long)cs_hash;
    t_sk_cur_tgy = group_count_y;
    t_sk_cur_u0hash = ck.desc;
    t_sk_cur_owner = ck.declared ? ck.owner : -1;
    t_sk_cur_owner_hash =
        (t_sk_cur_owner >= 0 && t_sk_cur_owner < (int)Mods().Count()) ? Mods().At(t_sk_cur_owner).ns_hash : 0;
    t_sk_cur_inst = ck.inst;
    if (ck.shapekey)
        PortApi::SetTypedObserver(
            [](const PortApi::ObservedCommand& oc, void*) {
                if (oc.post || oc.kind != PortApi::ObservedCommand::Kind::RunList || !oc.run_explicit) return;
                std::wstring owner = oc.run_target ? oc.run_target : std::wstring();
                ShapekeyQueue& q = g_sk_queue;
                std::lock_guard<std::mutex> l(q.pending_mutex_);
                const unsigned cur_tgy = t_sk_cur_tgy;
                ID3D12Resource* const cur_inst = t_sk_cur_inst;
                for (const auto& e : q.pending_)
                    if (e.list == oc.run_explicit && e.tgy == cur_tgy && e.inst == cur_inst) {
                        if (!t_q_count) t_q_reason = 4;
                        return;
                    } // already queued
                {
                    const int cur_owner = t_sk_cur_owner;
                    for (const auto& e : q.pending_) {
                        if (e.owner_ns_hash != t_sk_cur_owner_hash || e.inst != cur_inst) continue;
                        if (!PortApi::ListNests(e.list, oc.run_explicit)) continue;
                        if (g_debug &&
                            DiagChanged("SK-NESTED", cur_owner, (unsigned long long)(uintptr_t)oc.run_explicit)) {
                            logger::write("SK-NESTED: not queuing list %p -- an already queued list (%p) of the "
                                          "same owner (mod %d) reaches it, so it runs through that one at the drain",
                                oc.run_explicit, e.list, cur_owner);
                        }
                        return;
                    }
                }
                q.pending_.push_back(SkPendingList{ std::move(owner), oc.run_explicit, t_sk_cur_cs, t_sk_cur_u0hash,
                    t_sk_cur_tgy, t_sk_cur_owner_hash, cur_inst });
                ++t_q_count;
                t_q_reason = 0; // 0 = queued, which is the success case
            },
            nullptr);
}

void ShapekeyQueue::EndDispatchScope(bool shapekey)
{
    if (shapekey) PortApi::SetTypedObserver(nullptr, nullptr);
}

std::vector<SkPendingList> ShapekeyQueue::TakeOwnLists(const std::wstring& ns, int mod_id, ID3D12Resource* inst,
    MorphDump::DrainRecord& dr, size_t& left_for_others, size_t& dropped_unowned, size_t& other_inst_lists,
    ID3D12Resource*& other_inst)
{
    std::vector<SkPendingList> lists;
    auto owned_by_ns = [&](const std::wstring& key) {
        if (key.empty() || ns.empty()) return false;
        std::wstring k = key;
        std::wstring n = ns;
        for (wchar_t& c : k) c = (wchar_t)towlower(c);
        for (wchar_t& c : n) c = (wchar_t)towlower(c);
        return k.find(n) != std::wstring::npos;
    };
    const uint64_t own_hash = (mod_id >= 0 && mod_id < (int)Mods().Count()) ? Mods().At(mod_id).ns_hash : 0;
    std::lock_guard<std::mutex> l(pending_mutex_);
    dr.pending_in = (unsigned)pending_.size(); // the queue's depth BEFORE we filter it
    std::vector<SkPendingList> keep;
    for (auto& e : pending_) {
        bool mine;
        if (e.owner_ns_hash) {
            mine = (e.owner_ns_hash == own_hash);
        } else if (g_sk_toggles.strict_owner) {
            mine = false;
            ++dropped_unowned;
            continue; // dropped, NOT kept: nothing else will ever claim it either
        } else {
            mine = (e.u0hash && PortApi::ModDeclaresHash(ns.c_str(), e.u0hash)) || owned_by_ns(e.ns);
        }
        if (mine && e.inst && e.inst != inst) {
            mine = false;
            ++other_inst_lists;
            other_inst = e.inst;
        }
        if (mine)
            lists.push_back(e);
        else
            keep.push_back(std::move(e));
    }
    left_for_others = keep.size();
    pending_.swap(keep); // whatever we did not claim stays queued for its owner
    return lists;
}

void ShapekeyQueue::ForgetFor(ID3D12Resource* dead)
{
    std::lock_guard<std::mutex> l(pending_mutex_);
    pending_.erase(
        std::remove_if(pending_.begin(), pending_.end(), [dead](const SkPendingList& e) { return e.inst == dead; }),
        pending_.end());
}

void SkNoteDispatchList(const void* native_cl, uint32_t frame)
{
    g_sk_queue.NoteDispatchList(native_cl, frame);
}

void SkBeginDispatchScope(uint64_t cs_hash, uint32_t group_count_y, const SkU0Ident& ck)
{
    g_sk_queue.BeginDispatchScope(cs_hash, group_count_y, ck);
}

void SkEndDispatchScope(bool shapekey)
{
    g_sk_queue.EndDispatchScope(shapekey);
}

unsigned SkTakeQueueOutcome(uint8_t* reason)
{
    return g_sk_queue.TakeOutcome(reason);
}
