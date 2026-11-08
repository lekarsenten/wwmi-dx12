// The per-instance store: what a shapekey dispatch publishes for its instance's draw. See sk_internal.h.
#include "sk_internal.h"
#include "../instrumentation/profiler.h" // ProfGpuWork -- this file's GPU copies, for the performance report's work rows
#include "../instrumentation/removal_pin.h"
#include "../migoto/dx12_pipeline.h"
#include "../core/gpu_progress.h"
#include "../core/d3d12_name.h"
#include <cwctype>

ShapekeyToggles g_sk_toggles;
ShapekeyStats g_sk_stats;
ShapekeyStore g_sk_store;

// THE OWNERSHIP KEY, LOWERCASED IN EXACTLY ONE PLACE: a mod's namespace is the key for every resource the chain
// remembers between the dispatch that produced it and the point that consumes it.
std::wstring ShapekeyStore::NsKey(const std::wstring& ns)
{
    std::wstring k = ns;
    for (wchar_t& c : k) c = (wchar_t)towlower(c);
    return k;
}

// Look up an existing instance. Absent is a real answer, not a reason to create one.
SkNsState* ShapekeyStore::FindLocked(const std::wstring& ns, ID3D12Resource* inst)
{
    if (ns.empty() || !inst) return nullptr;
    auto it = store_.find(Key(NsKey(ns), inst));
    return it == store_.end() ? nullptr : &it->second; // node-based map: the pointer survives an insert
}

// Get or create. Only writers call this, and a writer naming a new instance is how an instance appears.
SkNsState* ShapekeyStore::EmplaceLocked(const std::wstring& ns, ID3D12Resource* inst)
{
    if (ns.empty() || !inst) return nullptr;
    return &store_[Key(NsKey(ns), inst)];
}

/* Resolve (kind, reg) to the binding it names, or nullptr. RegisterIndex is the single bounds check. */
static SkBufferBinding* SkBindingSlot(SkNsState& e, TargetClass kind, unsigned reg)
{
    const unsigned i = RegisterIndex(kind, reg);
    return i == kNoRegister ? nullptr : &e.reg[i];
}

static bool SkBindingLive(TargetClass kind, const SkBufferBinding& b)
{
    if (!b.res) return false;
    return RegisterBoundByAddress(kind) ? b.va != 0 : b.bytes != 0;
}

bool ShapekeyStore::GetBinding(
    const std::wstring& ns, ID3D12Resource* inst, TargetClass kind, unsigned reg, SkBufferBinding* out)
{
    std::lock_guard<std::mutex> lk(mutex_);
    const SkNsState* e = FindLocked(ns, inst);
    if (!e) return false;
    const SkBufferBinding* b = SkBindingSlot(*const_cast<SkNsState*>(e), kind, reg);
    if (!b || !SkBindingLive(kind, *b)) return false; /* nothing recorded is a real answer, not a default */
    *out = *b;
    return true;
}

void ShapekeyStore::RetireLocked(ID3D12Resource* r)
{
    if (r) grave_.emplace_back(r, GpuStamp());
}

void ShapekeyStore::SweepGraveLocked()
{
    /* destroy_resource, not Release: it also drops the buffer from ReShade's VA registry. */
    reshade::api::device* dev = g_addon.reshade_device;
    for (size_t k = 0; k < grave_.size();) {
        if (GpuPassed(grave_[k].second)) {
            if (dev)
                dev->destroy_resource(resource{ reinterpret_cast<uint64_t>(grave_[k].first) });
            else
                grave_[k].first->Release();
            grave_[k] = grave_.back();
            grave_.pop_back();
        } else
            ++k;
    }
}

static ID3D12Resource* SkMakeZeroSource(reshade::api::device* dev, unsigned bytes)
{
    if (!dev || !bytes) return nullptr;
    /* UPLOAD heap via cpu_access, which is GENERIC_READ (sdk d3d12_impl_type_convert.cpp:213). */
    resource h{};
    if (!dev->create_resource(resource_desc(bytes, memory_heap::upload, resource_usage::copy_source), nullptr,
            resource_usage::cpu_access, &h) ||
        !h.handle)
        return nullptr;
    void* mp = nullptr;
    if (!dev->map_buffer_region(h, 0, UINT64_MAX, map_access::write_only, &mp) || !mp) {
        dev->destroy_resource(h);
        return nullptr;
    }
    memset(mp, 0, bytes);
    dev->unmap_buffer_region(h);
    DevName(reinterpret_cast<ID3D12Resource*>(h.handle), L"wwmi shapekey zero source");
    return reinterpret_cast<ID3D12Resource*>(h.handle);
}

uint8_t ShapekeyStore::ZeroBinding(const std::wstring& ns, ID3D12Resource* inst, TargetClass kind, unsigned reg,
    reshade::api::command_list* cmd_list, const char* why)
{
    const unsigned i = RegisterIndex(kind, reg);
    if (i == kNoRegister) return 0;
    const void* cl = cmd_list ? reinterpret_cast<const void*>(cmd_list->get_native())
                              : nullptr; // the record path keys on the native list
    SkBufferBinding b{};
    if (!GetBinding(ns, inst, kind, reg, &b) || !b.res || !b.bytes) return 0; // nothing recorded: real answer
    const bool live = g_resources.Live(b.res);
    const unsigned identity = g_buffer_reloc.IdentityBytes(b.res, b.bytes);
    const unsigned hash = PortApi::GameComputeUavBufferHash(identity);
    const bool ours = live && PortApi::ModDeclaresHash(ns.c_str(), hash);
    ID3D12Resource* src = nullptr;
    bool need_build = false;
    /* DECIDE under the lock, BUILD outside it, INSTALL under it again. */
    {
        std::lock_guard<std::mutex> lk(mutex_);
        ZeroSource& z = zero_[i];
        if (!ours) {
            ++z.not_ours;
        } else if (cl) {
            if (z.buf && z.bytes < b.bytes) {
                RetireLocked(z.buf); // retired, NOT released -- see the note at grave_
                z.buf = nullptr;
                z.bytes = 0;
            }
            if (z.buf) {
                src = z.buf;
                ++z.cleared;
            } else
                need_build = true;
        }
        SweepGraveLocked();
    }
    if (ours && cl && need_build) {
        ID3D12Resource* nb = SkMakeZeroSource(g_addon.reshade_device, b.bytes);
        std::lock_guard<std::mutex> lk(mutex_);
        ZeroSource& z = zero_[i];
        if (!nb) {
            ++z.not_ready;
        } else if (z.buf && z.bytes >= b.bytes) {
            g_addon.reshade_device->destroy_resource(resource{ reinterpret_cast<uint64_t>(nb) });
            src = z.buf;
            ++z.cleared;
        } else {
            RetireLocked(z.buf);
            z.buf = nb;
            z.bytes = b.bytes;
            src = nb;
            ++z.cleared;
        }
    }
    if (!ours) {
        if (g_debug && LogBudget(why, ns, 6))
            logger::write("%s: REFUSED -- %c%u is %u B (identity %u B, hash %08x, live=%d), which ns='%ls' "
                          "does NOT declare. Not zeroing a buffer we cannot prove is ours.",
                why, TargetClassChar(kind), reg, b.bytes, identity, hash, (int)live, ns.c_str());
        return 2;
    }
    if (!cl) return 0;
    if (!src) return 3;
    {
        const resource dst{ reinterpret_cast<uint64_t>(b.res) };
        const resource zsrc{ reinterpret_cast<uint64_t>(src) };
        const bool owned = MigotoPort::OwnedBuffer(b.res);
        if (owned)
            MigotoPort::OwnedTransition(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native()), b.res,
                D3D12_RESOURCE_STATE_COPY_DEST);
        else if (g_sk_toggles.clear_barrier)
            cmd_list->barrier(dst, resource_usage::unordered_access, resource_usage::copy_dest);
        cmd_list->copy_buffer_region(zsrc, 0, dst, 0, b.bytes);
        ProfGpuWork(PGW_SkCopy, b.bytes);
        if (!owned && g_sk_toggles.clear_barrier)
            cmd_list->barrier(dst, resource_usage::copy_dest, resource_usage::unordered_access);
        RemovalPin::Ours(reinterpret_cast<const void*>(cmd_list->get_native()), "sk-clear");
        RemovalPin::Refs(reinterpret_cast<const void*>(cmd_list->get_native()), b.res, "sk-clear destination");
    }
    if (g_debug && LogBudget(why, ns, 6))
        logger::write("%s: zeroed %u B of %c%u (identity %u B, hash %08x, DECLARED) for ns='%ls' -- the "
                      "game's own per-dword clear does this to ITS buffer every frame; ours is a separate "
                      "resource it never sees.",
            why, b.bytes, TargetClassChar(kind), reg, identity, hash, ns.c_str());
    return 1;
}

void ShapekeyStore::ZeroStats(TargetClass kind, unsigned reg, unsigned long long* cleared, unsigned long long* not_ours,
    unsigned long long* not_ready)
{
    *cleared = *not_ours = *not_ready = 0;
    const unsigned i = RegisterIndex(kind, reg);
    if (i == kNoRegister) return;
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = zero_.find(i);
    if (it == zero_.end()) return;
    *cleared = it->second.cleared;
    *not_ours = it->second.not_ours;
    *not_ready = it->second.not_ready;
}

bool ShapekeyStore::SetSnap(const std::wstring& ns, ID3D12Resource* inst, unsigned reg, unsigned long long cs,
    unsigned tgy, ID3D12Resource* res, uint64_t va, unsigned bytes)
{
    const unsigned i = RegisterIndex(TargetClass::ConstantBuffer, reg);
    if (i == kNoRegister) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    SkNsState* e = EmplaceLocked(ns, inst);
    if (!e) return false;
    SkBufferBinding& b = e->snap[i][cs][tgy];
    b.res = res;
    b.va = va;
    b.bytes = bytes;
    return true;
}

bool ShapekeyStore::GetSnap(const std::wstring& ns, ID3D12Resource* inst, unsigned reg, unsigned long long cs,
    unsigned tgy, SkBufferBinding* out)
{
    const unsigned i = RegisterIndex(TargetClass::ConstantBuffer, reg);
    if (i == kNoRegister) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    SkNsState* e = FindLocked(ns, inst);
    if (!e) return false;
    auto si = e->snap.find(i);
    if (si == e->snap.end()) return false;
    auto ci = si->second.find(cs);
    if (ci == si->second.end()) return false;
    auto it = ci->second.find(tgy);
    if (it == ci->second.end() || !it->second.res || !it->second.va) return false;
    *out = it->second;
    return true;
}

void ShapekeyStore::SnapshotCb0(reshade::api::command_list* cmd_list, const std::wstring& ns, ID3D12Resource* inst,
    ID3D12Resource* src, uint64_t va, unsigned size, unsigned long long cs_hash, unsigned tgy)
{
    if (!g_sk_toggles.cb0_snapshot || !cmd_list || ns.empty() || !inst || !src || !va || !size) return;

    uint64_t base = 0;
    if (!g_resources.VaOf(reinterpret_cast<uint64_t>(src), &base)) return; // destroyed since resolve -- never deref
    const uint64_t off = (va > base) ? (va - base) : 0;

    const unsigned bytes = (size + 255u) & ~255u;
    if (bytes > 65536u || (off & 3u)) return;

    ID3D12Resource* snap = nullptr;
    ID3D12Resource* undersized = nullptr; // (`small` is a Windows macro)
    {
        SkBufferBinding prev{};
        if (GetSnap(ns, inst, 0, cs_hash, tgy, &prev)) {
            if (prev.bytes >= bytes)
                snap = prev.res;
            else
                undersized = prev.res;
        }
    }

    reshade::api::device* dev = cmd_list->get_device();
    if (!dev) return;

    if (!snap) {
        resource sh{};
        if (!dev->create_resource(resource_desc(bytes, memory_heap::default_,
                                      resource_usage::constant_buffer | resource_usage::copy_dest),
                nullptr, resource_usage::general, &sh) ||
            !sh.handle) {
            logger::write("CB0SNAP: create_resource FAILED for ns='%ls' (%u B) -- falling back to "
                          "the game's ring address, which is what was stale.",
                ns.c_str(), bytes);
            return;
        }
        snap = reinterpret_cast<ID3D12Resource*>(sh.handle);
        DevName(snap, L"wwmi cb snapshot");
        const uint64_t snap_gpu_va = snap->GetGPUVirtualAddress();
        if (undersized) {
            std::lock_guard<std::mutex> lk(mutex_);
            RetireLocked(undersized);
        }
        SetSnap(ns, inst, 0, cs_hash, tgy, snap, snap_gpu_va, bytes);
        if (g_debug)
            logger::write("CB0SNAP: ns='%ls' inst=%p snapshot %u B  src=%p base=%llx va=%llx off=%llu cs=%016llx%s",
                ns.c_str(), (void*)inst, bytes, (void*)src, (unsigned long long)base, (unsigned long long)va,
                (unsigned long long)off, cs_hash,
                off == 0 ? "  [off=0 -- b0 pointed at the RING BASE, not a suballocation]" : "");
    }

    const resource snap_h{ reinterpret_cast<uint64_t>(snap) };
    const resource src_h{ reinterpret_cast<uint64_t>(src) };
    cmd_list->copy_buffer_region(src_h, off, snap_h, 0, bytes);
    ProfGpuWork(PGW_SkCopy, bytes);
    RemovalPin::Ours(reinterpret_cast<const void*>(cmd_list->get_native()), "sk-snap");
    RemovalPin::Refs(reinterpret_cast<const void*>(cmd_list->get_native()), src, "sk-snap source");
}

void ShapekeyStore::SetState(const std::wstring& ns, ID3D12Resource* inst, const SkComputeState& st)
{
    std::lock_guard<std::mutex> lk(mutex_);
    SkNsState* e = EmplaceLocked(ns, inst);
    if (!e) return;
    for (unsigned i = 0; i < kRegisterFileTotal; ++i) e->reg[i] = st.reg[i];
    e->valid = st.valid;
}

bool ShapekeyStore::GetState(const std::wstring& ns, ID3D12Resource* inst, SkComputeState* out)
{
    std::lock_guard<std::mutex> lk(mutex_);
    SkNsState* e = FindLocked(ns, inst);
    if (!e || !e->valid) return false;
    for (unsigned i = 0; i < kRegisterFileTotal; ++i) out->reg[i] = e->reg[i];
    out->valid = true;
    return true;
}

void ShapekeyStore::Forget(ID3D12Resource* dead)
{
    if (!dead) return;
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto kv = store_.begin(); kv != store_.end();) {
        SkNsState& e = kv->second;
        if (kv->first.second == dead) {
            for (auto& per_reg : e.snap)
                for (auto& per_cs : per_reg.second)
                    for (auto& per_tgy : per_cs.second) RetireLocked(per_tgy.second.res);
            kv = store_.erase(kv);
            continue;
        }
        for (unsigned i = 0; i < kRegisterFileTotal; ++i)
            if (e.reg[i].res == dead) e.reg[i] = SkBufferBinding{};
        /* the snapshots are per queuing dispatch, so a dead one takes out its own key and no other */
        for (auto& per_reg : e.snap)
            for (auto& per_cs : per_reg.second)
                for (auto it = per_cs.second.begin(); it != per_cs.second.end();)
                    if (it->second.res == dead)
                        it = per_cs.second.erase(it);
                    else
                        ++it;
        ++kv;
    }
}

bool SkGetBinding(const std::wstring& ns, ID3D12Resource* inst, TargetClass kind, unsigned reg, SkBufferBinding* out)
{
    return g_sk_store.GetBinding(ns, inst, kind, reg, out);
}

void SkZeroStats(TargetClass kind, unsigned reg, unsigned long long* cleared, unsigned long long* not_ours,
    unsigned long long* not_ready)
{
    g_sk_store.ZeroStats(kind, reg, cleared, not_ours, not_ready);
}

void SkOnDestroyed(ID3D12Resource* dead)
{
    if (!dead) return;
    g_sk_store.Forget(dead);
    g_sk_queue.ForgetFor(dead);
}
