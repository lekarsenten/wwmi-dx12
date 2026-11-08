#pragma once
#include <reshade.hpp> // the pipeline_layout_param the walkers below take
#include <d3d12.h>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include "target_class.h" // RegisterFileWidth / TargetClass -- LayoutRegMap's array widths
#include "../instrumentation/diag_gate.h" // g_frame_id -- TableStartOf's memo

struct LayoutTableParam {
    uint8_t param;
    uint8_t kind;
    uint8_t is_srv;
    uint32_t count;
    uint32_t base_reg;
};

constexpr uint32_t kTexScanMin = 128;
constexpr uint32_t kTexScanMax = 512;

struct TableStart {
    uint64_t heap = 0;
    uint32_t base = 0;
    explicit operator bool() const { return heap != 0; }
    ID3D12DescriptorHeap* Heap() const { return reinterpret_cast<ID3D12DescriptorHeap*>(heap); }
};
inline TableStart TableStartOf(reshade::api::device* dev, reshade::api::descriptor_table tab)
{
    TableStart ts;
    if (!tab.handle) return ts;
    struct Memo {
        uint64_t table = 0;
        uint32_t frame = 0;
        TableStart ts;
    };
    thread_local Memo t_memo[16];
    thread_local unsigned t_next = 0;
    for (const Memo& m : t_memo)
        if (m.table == tab.handle && m.frame == g_frame_id) return m.ts;
    reshade::api::descriptor_heap dh = { 0 };
    dev->get_descriptor_heap_offset(tab, 0, 0, &dh, &ts.base);
    ts.heap = dh.handle;
    Memo& m = t_memo[t_next++ & 15];
    m.table = tab.handle;
    m.frame = g_frame_id;
    m.ts = ts;
    return ts;
}

struct CsCbvRoots {
    int reg[16];
};

struct U0CsTable {
    uint8_t param;
    uint32_t offset;
}; // ...or as (table param, descriptor offset)

struct RegBind {
    uint8_t param = 0xFF; // root parameter index
    uint32_t offset = 0; // descriptor offset WITHIN the table (0 for a root/push descriptor)
    bool is_table = false; // false = root descriptor (read from root[].va / CommandListRegistry::Cbv), true = table
};
struct LayoutRegMap {
    RegBind t_cs[RegisterFileWidth(TargetClass::ShaderResource)];
    RegBind u_cs[RegisterFileWidth(TargetClass::UnorderedAccessView)];
    RegBind b_cs[RegisterFileWidth(TargetClass::ConstantBuffer)]; // compute-visible
    RegBind t_gfx[RegisterFileWidth(TargetClass::ShaderResource)];
    RegBind u_gfx[RegisterFileWidth(TargetClass::UnorderedAccessView)];
    RegBind b_gfx[RegisterFileWidth(TargetClass::ConstantBuffer)]; // graphics-visible (vs/ps/...)
    RegBind t_vs[RegisterFileWidth(TargetClass::ShaderResource)];
    RegBind t_ps[RegisterFileWidth(TargetClass::ShaderResource)]; // per stage: the frame dump's vs-tN / ps-tN
    RegBind b_vs[RegisterFileWidth(TargetClass::ConstantBuffer)]; // per stage: the game declares one root
    RegBind b_ps[RegisterFileWidth(TargetClass::ConstantBuffer)];
    bool built = false;
};

// What every root signature the game creates actually declares: which root parameter carries which shader
// register, which parameters are tables and what each table holds. Walked once at creation; read at every draw
// and dispatch that would otherwise have to guess.
class LayoutRegistry {
public:
    void Register(uint32_t count, const reshade::api::pipeline_layout_param* params, uint64_t layout);
    void Forget(uint64_t layout);

    // The root parameters of the skeleton CBs, b4 (current) and b3 (previous): written only when the layout
    // declares them.
    void BoneRoots(uint64_t layout, int* b4, int* b3);
    // The drawn object's own CB (vs-b2) as a root parameter, -1 when none.
    int ObjectCbRoot(uint64_t layout);
    // The compute-visible cb0 and u0 as root parameters, -1 when none.
    int CsCb0Root(uint64_t layout);
    int CsU0Root(uint64_t layout);
    // The compute u0 as a table entry; false when the layout has none (*out untouched).
    bool U0Table(uint64_t layout, U0CsTable* out);
    // The compute-visible cb0..cb15 root parameters; false when the layout declares none.
    bool CsCbvRootsOf(uint64_t layout, CsCbvRoots* out);
    // How many descriptors of each table parameter a texture scan may read: the declared count, within bounds.
    void TexScanLens(uint64_t layout, uint32_t (&out)[64]);
    // f(const std::vector<LayoutTableParam>&) under the lock; false when the layout has no table record.
    template <class F> bool WithTables(uint64_t layout, F f)
    {
        std::lock_guard l(tables_mutex_);
        auto it = tables_.find(layout);
        if (it == tables_.end()) return false;
        f(it->second);
        return true;
    }
    // f(const LayoutRegMap&) under the lock; false when the layout has no register map.
    template <class F> bool WithRegMap(uint64_t layout, F f)
    {
        std::lock_guard l(regmap_mutex_);
        auto it = regmap_.find(layout);
        if (it == regmap_.end() || !it->second.built) return false;
        f(it->second);
        return true;
    }

private:
    static int RootOf(const std::unordered_map<uint64_t, int>& m, uint64_t layout); // under mutex_

    std::mutex mutex_; // guards the root maps below
    std::unordered_map<uint64_t, int> b4_root_; // CBV b4 -- the CURRENT skeleton
    std::unordered_map<uint64_t, int> b3_root_; // ...and the previous one
    std::unordered_map<uint64_t, int> b2_root_; // CBV b2 -- the drawn object's own: one address per copy
    std::unordered_map<uint64_t, int> b0cs_root_; // ...the compute-visible one
    std::unordered_map<uint64_t, CsCbvRoots> cs_cbv_roots_; // b0..b15, compute
    std::unordered_map<uint64_t, int> u0cs_root_; // compute UAV u0 as a ROOT param
    std::unordered_map<uint64_t, U0CsTable> u0cs_table_;
    std::unordered_map<uint64_t, int> dead_; // a destroyed layout's b4 root, for the recycle report
    uint32_t recycled_ = 0;
    std::mutex tables_mutex_;
    std::unordered_map<uint64_t, std::vector<LayoutTableParam>> tables_;
    std::mutex regmap_mutex_;
    std::unordered_map<uint64_t, LayoutRegMap> regmap_;
};
extern LayoutRegistry g_layouts;

void on_init_pipeline_layout(reshade::api::device*, uint32_t count, const reshade::api::pipeline_layout_param* params,
    reshade::api::pipeline_layout layout);
void on_destroy_pipeline_layout(reshade::api::device*, reshade::api::pipeline_layout layout);
