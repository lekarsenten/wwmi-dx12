#pragma once
#include <d3d12.h>
#include <intrin.h>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <algorithm>
#include <new>
#include <vector>
#include <reshade.hpp>
#include "../core/sharded_map.h"

struct ResInfo {
    uint32_t size;
    ID3D12Resource* res;
    uint64_t cached_va;
    bool tex_cand;
    uint32_t view_off;
    uint64_t view_handle;
    bool is_buffer;
    uint32_t wrote_frame = 0; // g_frame_id when this slot was last written
    uint8_t wrote_src = 0; // 0 = never/unknown, 1 = update_descriptor_tables,
    // 2 = copy_descriptor_tables, 3 = a view created in the slot
    ID3D12Resource* swapped = nullptr;
    uint32_t miss_frame = 0;
};
struct HeapKey {
    uint64_t heap;
    uint32_t offset;
    bool operator==(const HeapKey& o) const { return heap == o.heap && offset == o.offset; }
};
struct HeapKeyHash {
    size_t operator()(const HeapKey& k) const
    {
        return std::hash<uint64_t>{}(k.heap) ^ std::hash<uint32_t>{}(k.offset);
    }
};

// View handle -> the resource it views, lock-free, paged by descriptor address.
struct PagedViewMap {
    static constexpr uint64_t kPage = 4096;
    struct Rec {
        std::atomic<uint64_t> key;
        std::atomic<uint32_t> seq;
        ResInfo val;
    }; // seq: the map's seq at the last put / forget
    struct Dir {
        std::atomic<uint64_t> page;
        std::atomic<Rec*> recs;
    };
    struct DirTab {
        uint32_t cap;
        Dir* d;
    };
    std::atomic<DirTab*> dirt{ new DirTab{ 1u << 16, new Dir[1u << 16]() } };
    std::mutex add;
    uint32_t used = 0; // under add
    std::atomic<uint32_t> shift{ 0 }; // log2(granularity); 0 = not initialised
    std::atomic<uint32_t> seq{ 1 }; // bumped per put / forget; a heap slot remembers the value it was written at
    void init(ID3D12Device* dev)
    {
        if (shift.load(std::memory_order_acquire) || !dev) return;
        UINT g = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g = (std::min)(g, dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
        g = (std::min)(g, dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV));
        uint32_t s = 1;
        while ((1u << (s + 1)) <= g && s < 11) ++s; // floor(log2 g), at least 2 bytes
        shift.store(s, std::memory_order_release);
    }
    uint32_t per_page() const { return (uint32_t)(kPage >> shift.load(std::memory_order_relaxed)); }
    static uint32_t dir_hash(uint64_t p, uint32_t cap)
    {
        p *= 0x9E3779B97F4A7C15ull;
        return (uint32_t)(p >> 32) & (cap - 1);
    }
    static Rec* find_page(const DirTab* t, uint64_t page)
    {
        for (uint32_t i = dir_hash(page, t->cap);; i = (i + 1) & (t->cap - 1)) {
            const uint64_t k = t->d[i].page.load(std::memory_order_acquire);
            if (k == page) return t->d[i].recs.load(std::memory_order_acquire);
            if (k == 0) return nullptr;
        }
    }
    static void put_page(DirTab* t, uint64_t page, Rec* recs)
    { // under add; recs before page
        uint32_t i = dir_hash(page, t->cap);
        while (t->d[i].page.load(std::memory_order_relaxed)) i = (i + 1) & (t->cap - 1);
        t->d[i].recs.store(recs, std::memory_order_relaxed);
        t->d[i].page.store(page, std::memory_order_release);
    }
    Rec* page_of(uint64_t page, bool create)
    {
        struct Hit {
            uint64_t page;
            Rec* recs;
            const PagedViewMap* map;
        };
        thread_local Hit t_hit[256] = {};
        Hit& h = t_hit[(page ^ (page >> 8)) & 255];
        if (h.page == page && h.map == this && h.recs) return h.recs;
        Rec* r = find_page(dirt.load(std::memory_order_acquire), page);
        if (!r && create) {
            std::lock_guard l(add);
            DirTab* t = dirt.load(std::memory_order_relaxed);
            r = find_page(t, page);
            if (!r) {
                Rec* fresh = new (std::nothrow) Rec[per_page()]();
                if (!fresh) return nullptr;
                if ((used + 1) * 2 > t->cap) {
                    DirTab* g = new DirTab{ t->cap * 2, new Dir[t->cap * 2]() };
                    for (uint32_t i = 0; i < t->cap; ++i)
                        if (const uint64_t k = t->d[i].page.load(std::memory_order_relaxed))
                            put_page(g, k, t->d[i].recs.load(std::memory_order_relaxed));
                    dirt.store(g, std::memory_order_release);
                    t = g;
                }
                put_page(t, page, fresh);
                ++used;
                r = fresh;
            }
        }
        if (r) h = { page, r, this };
        return r;
    }
    Rec* rec_of(uint64_t key, bool create)
    {
        const uint32_t s = shift.load(std::memory_order_acquire);
        if (!key || !s) return nullptr;
        Rec* recs = page_of(key / kPage, create);
        return recs ? recs + ((key % kPage) >> s) : nullptr;
    }
    // The record at `key`, if it exists and -- with `at_seq` -- was made no later than at_seq (wrap-safe).
    bool find_seq(uint64_t key, uint32_t at_seq, ResInfo& out)
    {
        Rec* r = rec_of(key, false);
        if (!r) return false;
        if (r->key.load(std::memory_order_acquire) != key) return false;
        if (at_seq && (int32_t)(r->seq.load(std::memory_order_acquire) - at_seq) > 0) return false;
        ID3D12Resource* res = std::atomic_ref<ID3D12Resource*>(r->val.res).load(std::memory_order_acquire);
        if (!res) return false;
        out = r->val;
        out.res = res; // torn non-res fields tolerated
        if (r->key.load(std::memory_order_acquire) != key) return false; // re-keyed meanwhile: a miss
        return !at_seq || (int32_t)(r->seq.load(std::memory_order_acquire) - at_seq) <= 0;
    }
    bool find(uint64_t key, ResInfo& out) { return find_seq(key, 0, out); }
    uint32_t current_seq() const { return seq.load(std::memory_order_acquire); }
    uint32_t next_seq()
    {
        uint32_t s = seq.fetch_add(1, std::memory_order_acq_rel) + 1;
        return s ? s : seq.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    void put(uint64_t key, const ResInfo& v)
    {
        Rec* r = rec_of(key, true);
        if (!r) return;
        std::atomic_ref<ID3D12Resource*>(r->val.res).store(nullptr, std::memory_order_release); // unpublish first
        r->key.store(key, std::memory_order_release);
        ResInfo& e = r->val;
        e.size = v.size;
        e.cached_va = v.cached_va;
        e.tex_cand = v.tex_cand;
        e.view_off = v.view_off;
        e.view_handle = v.view_handle;
        e.is_buffer = v.is_buffer;
        e.wrote_frame = v.wrote_frame;
        e.wrote_src = v.wrote_src;
        r->seq.store(next_seq(), std::memory_order_release);
        std::atomic_ref<ID3D12Resource*>(e.res).store(v.res, std::memory_order_release); // publish res LAST
    }
    void forget(uint64_t key)
    {
        Rec* r = rec_of(key, false);
        if (!r) return;
        if (r->key.load(std::memory_order_acquire) != key) return;
        std::atomic_ref<ID3D12Resource*>(r->val.res).store(nullptr, std::memory_order_release);
        r->seq.store(next_seq(), std::memory_order_release);
    }
};

// Descriptor heap -> what each of its slots holds: the view it was written from and the mod texture stamped over
// it. A flat slot array per heap, lock-free: the hot path.
struct FlatHeapMap {
    static constexpr uint64_t kDead = 1; // an entry whose heap died; reusable once its array is freed
    static constexpr uint64_t kGraveMs = 2000;
    struct Slot {
        std::atomic<uint64_t> src{ 0 }; // the CPU handle the view this slot holds was created at
        std::atomic<ID3D12Resource*> swapped{ nullptr }; // the mod texture stamped over this slot's descriptor
        std::atomic<uint32_t> seq{ 0 }; // the view map's seq when written; 0 = empty
        uint32_t miss_frame = 0;
        uint32_t frame = 0; // g_frame_id when written
        uint8_t src_kind = 0; // ResInfo::wrote_src
    };
    static constexpr uint32_t kMarkShift = 6; // a mark bit per block of 64 slots
    struct Entry {
        std::atomic<uint64_t> id{ 0 }; // heap handle; 0 = free, kDead = heap destroyed, array not yet freed
        std::atomic<Slot*> arr{
            nullptr
        }; // its flat slot array; null for a heap that holds no views of ours (samplers, RTV, DSV)
        std::atomic<std::atomic<uint64_t>*> marks{ nullptr }; // with arr: a set bit = its block may hold a stamped slot
        std::atomic<uint32_t> sz{ 0 }; // its length (NumDescriptors); 0 with a null array
        std::atomic<uint8_t> vis{ 0 }; // 1 = shader-visible
        std::atomic<uint64_t> lo{ 0 }; // its CPU descriptor handle start
    };
    static uint32_t MarkWords(uint32_t n) { return (n + (64u << kMarkShift) - 1) / (64u << kMarkShift); }
    struct Block {
        Entry e[32];
        std::atomic<Block*> next{ nullptr };
    };
    Block head;
    std::atomic<uint32_t> rejected{ 0 }; // writes to a heap over 2M descriptors (not tracked)
    std::atomic<uint32_t> destroyed{ 0 };
    std::atomic<uint32_t> published{ 0 }; // entries published so far: a remembered miss holds until it moves
    std::mutex add_mtx; // adding / retiring a heap (rare)
    std::atomic<void (*)(uint64_t)> retire_hook{ nullptr }; // told when a heap is retired (outside add_mtx)
    struct Grave {
        Entry* e;
        Slot* arr;
        std::atomic<uint64_t>* marks;
        uint64_t t;
    };
    std::vector<Grave> graves; // under add_mtx

    template <class F> Entry* scan(F f)
    {
        for (Block* b = &head; b; b = b->next.load(std::memory_order_acquire))
            for (Entry& e : b->e)
                if (f(e)) return &e;
        return nullptr;
    }
    Entry* entry_of(uint64_t heap)
    {
        struct Hit {
            uint64_t heap;
            Entry* e;
            uint32_t added;
        };
        thread_local Hit t_hit[256] = {};
        const uint32_t now = published.load(std::memory_order_acquire);
        Hit& h = t_hit[((heap >> 6) ^ (heap >> 14)) & 255];
        if (h.heap == heap && (h.e ? h.e->id.load(std::memory_order_acquire) == heap : h.added == now)) return h.e;
        Entry* e = scan([heap](Entry& x) { return x.id.load(std::memory_order_acquire) == heap; });
        h = { heap, e, now };
        return e;
    }
    void bury_due(uint64_t now)
    { // under add_mtx
        for (size_t i = 0; i < graves.size();) {
            if (now - graves[i].t < kGraveMs) {
                ++i;
                continue;
            }
            delete[] graves[i].arr;
            delete[] graves[i].marks;
            graves[i].e->id.store(0, std::memory_order_release);
            graves[i] = graves.back();
            graves.pop_back();
        }
    }
    void watch(uint64_t heap); // below: the death watch
    void on_destroyed(uint64_t heap)
    {
        {
            std::lock_guard l(add_mtx);
            Entry* e = entry_of(heap);
            if (!e) return;
            Slot* arr = e->arr.load(std::memory_order_relaxed);
            e->id.store(kDead, std::memory_order_release); // lookups miss from here on
            destroyed.fetch_add(1, std::memory_order_relaxed);
            graves.push_back({ e, arr, e->marks.load(std::memory_order_relaxed), GetTickCount64() });
            bury_due(GetTickCount64());
        }
        if (auto h = retire_hook.load(std::memory_order_acquire)) h(heap);
    }
    Entry* enter(uint64_t heap)
    {
        Entry* e = entry_of(heap);
        if (e) return e;
        bool added = false;
        {
            std::lock_guard l(add_mtx);
            e = entry_of(heap);
            if (!e) {
                bury_due(GetTickCount64());
                const D3D12_DESCRIPTOR_HEAP_DESC hd = reinterpret_cast<ID3D12DescriptorHeap*>(heap)->GetDesc();
                const bool views = hd.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                const uint32_t n = views ? hd.NumDescriptors : 0;
                if (views && (!n || n > 2000000u)) {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                    return nullptr;
                }
                Slot* arr = n ? new (std::nothrow) Slot[n]() : nullptr;
                std::atomic<uint64_t>* marks = n ? new (std::nothrow) std::atomic<uint64_t>[MarkWords(n)]() : nullptr;
                if (n && (!arr || !marks)) {
                    delete[] arr;
                    delete[] marks;
                    return nullptr;
                }
                Entry* f = scan([](Entry& x) { return x.id.load(std::memory_order_relaxed) == 0; });
                if (!f) {
                    Block* b = &head;
                    while (Block* nb = b->next.load(std::memory_order_relaxed)) b = nb;
                    Block* nb = new Block();
                    f = &nb->e[0];
                    b->next.store(nb, std::memory_order_release);
                }
                f->sz.store(n, std::memory_order_relaxed);
                f->vis.store((hd.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? 1 : 0, std::memory_order_relaxed);
                f->lo.store(reinterpret_cast<ID3D12DescriptorHeap*>(heap)->GetCPUDescriptorHandleForHeapStart().ptr,
                    std::memory_order_relaxed);
                f->arr.store(arr, std::memory_order_relaxed);
                f->marks.store(marks, std::memory_order_relaxed);
                f->id.store(heap, std::memory_order_release); // publish LAST
                published.fetch_add(1, std::memory_order_acq_rel);
                e = f;
                added = true;
            }
        }
        if (added) watch(heap);
        return e;
    }
    static Slot* at(Entry* e, uint32_t offset)
    {
        if (!e || offset >= e->sz.load(std::memory_order_relaxed)) return nullptr;
        return e->arr.load(std::memory_order_relaxed) + offset;
    }
    Slot* slot(uint64_t heap, uint32_t offset, bool create)
    {
        return at(create ? enter(heap) : entry_of(heap), offset);
    }
    bool shader_visible(uint64_t heap)
    {
        const Entry* e = entry_of(heap);
        return e && e->vis.load(std::memory_order_relaxed) != 0;
    }
    // f(heap, cpu_start, count, shader_visible, slots) for every live heap; true from f stops the walk.
    template <class F> void for_each_heap(F f)
    {
        scan([&](Entry& e) {
            const uint64_t h = e.id.load(std::memory_order_acquire);
            if (h <= kDead) return false;
            return (bool)f(h, e.lo.load(std::memory_order_relaxed), e.sz.load(std::memory_order_relaxed),
                e.vis.load(std::memory_order_relaxed) != 0, e.arr.load(std::memory_order_relaxed));
        });
    }
    // The slot's view record, if the record at its handle was made before the slot was written.
    bool find_in(PagedViewMap& views, const Entry* e, uint32_t offset, ResInfo& out)
    {
        const Slot* p = at(const_cast<Entry*>(e), offset);
        if (!p) return false;
        const uint32_t seq = p->seq.load(std::memory_order_acquire);
        if (!seq) return false;
        const uint64_t h = p->src.load(std::memory_order_relaxed);
        if (!views.find_seq(h, seq, out)) return false;
        out.swapped = p->swapped.load(std::memory_order_acquire);
        out.miss_frame = p->miss_frame;
        out.wrote_frame = p->frame;
        out.wrote_src = p->src_kind;
        // rewritten meanwhile: a miss (two writes of one frame may share a seq, so the handle is checked too)
        return p->seq.load(std::memory_order_acquire) == seq && p->src.load(std::memory_order_relaxed) == h;
    }
    bool find(PagedViewMap& views, const HeapKey& k, ResInfo& out)
    {
        return find_in(views, entry_of(k.heap), k.offset, out);
    }
    // The slot holds the view created at `src` (its own handle for a view made or placed here), as of `seq`.
    static void set_at(Slot* p, uint64_t src, uint32_t seq, uint32_t frame, uint8_t kind, ID3D12Resource* swapped)
    {
        if (!p) return;
        p->seq.store(0, std::memory_order_release); // unpublish first
        p->src.store(src, std::memory_order_relaxed);
        p->frame = frame;
        p->src_kind = kind;
        p->miss_frame = 0;
        p->swapped.store(swapped, std::memory_order_relaxed);
        p->seq.store(seq, std::memory_order_release); // publish LAST
    }
    static void erase_at(Slot* p)
    {
        if (p) p->seq.store(0, std::memory_order_release);
    }
    void erase(const HeapKey& k) { erase_at(slot(k.heap, k.offset, false)); }
    // After a slot's swapped is set: its block marked, after the store, so a walk that unmarks it before looking sees it.
    static void mark(Entry* e, uint32_t offset)
    {
        std::atomic<uint64_t>* m = e ? e->marks.load(std::memory_order_relaxed) : nullptr;
        if (m) m[offset >> (kMarkShift + 6)].fetch_or(1ull << ((offset >> kMarkShift) & 63), std::memory_order_release);
    }
    void set_swapped(const HeapKey& k, ID3D12Resource* r)
    {
        Entry* e = entry_of(k.heap);
        Slot* p = at(e, k.offset);
        if (!p) return;
        p->miss_frame = 0; // a stamp supersedes an earlier miss on the slot
        p->swapped.store(r, std::memory_order_release);
        if (r) mark(e, k.offset);
    }
    // f(heap, cpu_start, shader_visible, offset, slot) for every slot holding a stamp, found through the marks: a
    // block is unmarked before it is looked at and marked again if a stamp is left in it.
    template <class F> void for_each_stamped(F f)
    {
        scan([&](Entry& e) {
            const uint64_t h = e.id.load(std::memory_order_acquire);
            if (h <= kDead) return false;
            Slot* arr = e.arr.load(std::memory_order_relaxed);
            std::atomic<uint64_t>* marks = e.marks.load(std::memory_order_relaxed);
            const uint32_t n = e.sz.load(std::memory_order_relaxed);
            if (!arr || !marks) return false;
            const uint64_t lo = e.lo.load(std::memory_order_relaxed);
            const bool vis = e.vis.load(std::memory_order_relaxed) != 0;
            for (uint32_t w = 0, words = MarkWords(n); w < words; ++w) {
                for (uint64_t bits = marks[w].load(std::memory_order_acquire); bits; bits &= bits - 1) {
                    unsigned long b = 0;
                    _BitScanForward64(&b, bits);
                    const uint64_t bit = 1ull << b;
                    marks[w].fetch_and(~bit, std::memory_order_acq_rel);
                    const uint32_t first = ((w << 6) + (uint32_t)b) << kMarkShift;
                    const uint32_t last = (std::min)(first + (1u << kMarkShift), n);
                    bool left = false;
                    for (uint32_t o = first; o < last; ++o) {
                        if (!arr[o].swapped.load(std::memory_order_acquire)) continue;
                        f(h, lo, vis, o, &arr[o]);
                        left = left || arr[o].swapped.load(std::memory_order_acquire);
                    }
                    if (left) marks[w].fetch_or(bit, std::memory_order_release);
                }
            }
            return false;
        });
    }
    void set_miss(const HeapKey& k, uint32_t frame)
    {
        Slot* p = slot(k.heap, k.offset, false);
        if (!p) return;
        p->miss_frame = frame;
    }
    // Every slot forgotten; the arrays stay (a reader may be inside one).
    void clear_all()
    {
        for_each_heap([](uint64_t, uint64_t, uint32_t n, bool, Slot* arr) {
            for (uint32_t o = 0; o < n; ++o) {
                arr[o].seq.store(0, std::memory_order_release);
                arr[o].swapped.store(nullptr, std::memory_order_release);
            }
            return false;
        });
    }
    void unwatch_all();
};

struct HeapDeathWatch final : IUnknown {
    static constexpr GUID kGuid = { 0x6b1f0f52, 0x3d8e, 0x4c6a, { 0x9b, 0x21, 0x5e, 0x7a, 0x40, 0x11, 0xc3, 0x8d } };
    std::atomic<ULONG> rc{ 1 };
    FlatHeapMap* map;
    uint64_t heap;
    HeapDeathWatch(FlatHeapMap* m, uint64_t h)
        : map(m),
          heap(h)
    {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return rc.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = rc.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) {
            map->on_destroyed(heap);
            delete this;
        }
        return n;
    }
};
inline void FlatHeapMap::watch(uint64_t heap)
{
    auto* w = new HeapDeathWatch(this, heap);
    if (FAILED(reinterpret_cast<ID3D12DescriptorHeap*>(heap)->SetPrivateDataInterface(HeapDeathWatch::kGuid, w)))
        on_destroyed(heap); // untrackable lifetime: never keep a heap we cannot see die
    w->Release();
}
inline void FlatHeapMap::unwatch_all()
{
    std::vector<uint64_t> heaps;
    for_each_heap([&](uint64_t h, uint64_t, uint32_t, bool, Slot*) {
        heaps.push_back(h);
        return false;
    });
    for (uint64_t h : heaps)
        reinterpret_cast<ID3D12DescriptorHeap*>(h)->SetPrivateDataInterface(HeapDeathWatch::kGuid, nullptr);
}

struct HeapCbv {
    ID3D12Resource* res = nullptr;
    uint64_t va = 0;
    uint32_t size = 0;
};

// The shadow of the game's descriptor heaps: which view each slot holds (and so which resource), which slots a
// mod texture was stamped over, and the CBVs written into them. Written by the descriptor hooks, read at draws.
class DescriptorShadow {
public:
    // The descriptor hooks' work.
    void ViewCreated(reshade::api::device* device, reshade::api::resource res, reshade::api::resource_view view);
    void DescriptorsWritten(
        reshade::api::device* device, uint32_t count, const reshade::api::descriptor_table_update* updates);
    void DescriptorsCopied(
        reshade::api::device* device, uint32_t count, const reshade::api::descriptor_table_copy* copies);

    // What a heap slot holds: its view record, the mod texture stamped over it, and when it was written.
    bool Find(const HeapKey& key, ResInfo& out);
    bool FindIn(FlatHeapMap::Entry* e, uint32_t offset, ResInfo& out);
    // The resource a view handle was created on.
    bool FindView(uint64_t view, ResInfo& out);
    FlatHeapMap::Entry* EntryOf(uint64_t heap);
    bool FindCbv(const HeapKey& key, HeapCbv& out);
    // The tracked heap slot a CPU descriptor handle points into. False = no heap we track holds it.
    bool SlotOfCpu(uint64_t cpu, HeapKey* out);
    bool ShaderVisible(uint64_t heap);
    void SetSwapped(const HeapKey& key, ID3D12Resource* res);
    void SetMiss(const HeapKey& key, uint32_t frame);
    void SetRetireHook(void (*hook)(uint64_t heap));
    template <class F> void ForEachHeap(F f) { heaps_.for_each_heap(f); }
    template <class F> void ForEachStamped(F f) { heaps_.for_each_stamped(f); }
    // Every slot and CBV record forgotten: the add-on was bypassed and they went stale.
    void Clear();
    // No heap's release may call into this DLL once it is unmapped.
    void UnwatchAll();

private:
    void TrackView(reshade::api::device* device, reshade::api::resource_view view);
    void RecordViewInTrackedHeap(reshade::api::resource res, reshade::api::resource_view view);

    PagedViewMap views_;
    FlatHeapMap heaps_;
    ShardedMap<HeapKey, HeapCbv, HeapKeyHash> cbvs_;
};
extern DescriptorShadow g_descriptors;

void on_init_resource_view(reshade::api::device* device, reshade::api::resource res,
    reshade::api::resource_usage usage_type, const reshade::api::resource_view_desc& desc,
    reshade::api::resource_view view);
bool on_update_descriptor_tables(
    reshade::api::device* device, uint32_t count, const reshade::api::descriptor_table_update* updates);
bool on_copy_descriptor_tables(
    reshade::api::device* device, uint32_t count, const reshade::api::descriptor_table_copy* copies);
