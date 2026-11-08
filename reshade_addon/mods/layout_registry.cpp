// THE LAYOUT REGISTRY -- what every root signature the game creates actually declares. See layout_registry.h.
#include "../addon_common.h"
#include "layout_registry.h"
#include "../addon/addon.h"
#include "../hooks/command_list_registry.h"

LayoutRegistry g_layouts;

static uint32_t LayoutParamRangeCount(const pipeline_layout_param& p)
{
    switch (p.type) {
    case pipeline_layout_param_type::push_descriptors: return 1;
    case pipeline_layout_param_type::descriptor_table:
    case pipeline_layout_param_type::push_descriptors_with_ranges:
        return p.descriptor_table.ranges ? p.descriptor_table.count : 0u;
    case pipeline_layout_param_type::descriptor_table_with_flags:
    case pipeline_layout_param_type::push_descriptors_with_ranges_and_flags:
        return p.descriptor_table_with_flags.ranges ? p.descriptor_table_with_flags.count : 0u;
    default: return 0; // push_constants carries no descriptor range
    }
}

static bool LayoutParamRange(const pipeline_layout_param& p, uint32_t i, descriptor_range& out,
    bool* is_table = nullptr, descriptor_range_flags* flags = nullptr)
{
    if (flags) *flags = descriptor_range_flags::none;
    if (i >= LayoutParamRangeCount(p)) return false;
    switch (p.type) {
    case pipeline_layout_param_type::push_descriptors:
        out = p.push_descriptors;
        if (is_table) *is_table = false;
        return true;
    case pipeline_layout_param_type::descriptor_table:
    case pipeline_layout_param_type::push_descriptors_with_ranges:
        out = p.descriptor_table.ranges[i];
        if (is_table) *is_table = (p.type == pipeline_layout_param_type::descriptor_table);
        return true;
    case pipeline_layout_param_type::descriptor_table_with_flags:
    case pipeline_layout_param_type::push_descriptors_with_ranges_and_flags: {
        const descriptor_range_with_flags& rf = p.descriptor_table_with_flags.ranges[i];
        out = static_cast<const descriptor_range&>(rf);
        if (flags && g_addon.reshade_api == RESHADE_API_VERSION) *flags = rf.flags;
    }
        if (is_table) *is_table = (p.type == pipeline_layout_param_type::descriptor_table_with_flags);
        return true;
    default: return false;
    }
}

static inline const char* RangeFlagsStr(descriptor_range_flags f, char buf[8])
{
    int n = 0;
    if ((uint32_t)(f & descriptor_range_flags::descriptors_volatile)) buf[n++] = 'V';
    if ((uint32_t)(f & descriptor_range_flags::data_volatile)) buf[n++] = 'd';
    if ((uint32_t)(f & descriptor_range_flags::data_static_while_set_at_execute)) buf[n++] = 'e';
    if ((uint32_t)(f & descriptor_range_flags::data_static)) buf[n++] = 'S';
    if ((uint32_t)(f & descriptor_range_flags::partially_bound)) buf[n++] = 'P';
    if (n == 0) buf[n++] = '-';
    buf[n] = 0;
    return buf;
}

// True when this parameter is a ROOT (push) descriptor rather than a heap-backed descriptor table.
static bool LayoutParamIsRootDescriptor(const pipeline_layout_param& p)
{
    return p.type == pipeline_layout_param_type::push_descriptors ||
        p.type == pipeline_layout_param_type::push_descriptors_with_ranges ||
        p.type == pipeline_layout_param_type::push_descriptors_with_ranges_and_flags;
}

static int FindVsCbvRoot(uint32_t count, const pipeline_layout_param* params, uint32_t reg)
{
    for (uint32_t i = 0; i < count; ++i) {
        const auto& p = params[i];
        if (!LayoutParamIsRootDescriptor(p)) continue; // b4 must be a ROOT CBV: callers read CommandListRegistry::Cbv
        descriptor_range r;
        if (!LayoutParamRange(p, 0, r)) continue;
        if (r.type != descriptor_type::constant_buffer || r.dx_register_space != 0 || r.dx_register_index != reg)
            continue;
        if (((uint32_t)(r.visibility & shader_stage::vertex) != 0) || (r.visibility == shader_stage::all))
            return (int)i; // vertex-visible -> this is the skinning skeleton CB
    }
    return -1;
}

static int FindCsCbvRoot(uint32_t count, const pipeline_layout_param* params, uint32_t reg)
{
    for (uint32_t i = 0; i < count; ++i) {
        const auto& p = params[i];
        if (!LayoutParamIsRootDescriptor(p)) continue;
        descriptor_range r;
        if (!LayoutParamRange(p, 0, r)) continue;
        if (r.type != descriptor_type::constant_buffer || r.dx_register_space != 0 || r.dx_register_index != reg)
            continue;
        if (r.visibility == shader_stage::all || ((uint32_t)(r.visibility & shader_stage::compute) != 0)) return (int)i;
    }
    return -1;
}

static int FindCsUavRoot(uint32_t count, const pipeline_layout_param* params, uint32_t reg)
{
    for (uint32_t i = 0; i < count; ++i) {
        const auto& p = params[i];
        if (!LayoutParamIsRootDescriptor(p)) continue;
        descriptor_range r;
        if (!LayoutParamRange(p, 0, r)) continue;
        if (r.type != descriptor_type::unordered_access_view || r.dx_register_space != 0 || r.dx_register_index != reg)
            continue;
        if (r.visibility == shader_stage::all || ((uint32_t)(r.visibility & shader_stage::compute) != 0)) return (int)i;
    }
    return -1;
}

// Which (root param, table offset) feeds compute UAV register `reg`, walking the declared ranges.
static bool FindCsUavTable(uint32_t count, const pipeline_layout_param* params, uint32_t reg, U0CsTable* out)
{
    for (uint32_t i = 0; i < count && i < 64; ++i) {
        const auto& p = params[i];
        if (LayoutParamIsRootDescriptor(p) || LayoutParamRangeCount(p) == 0) continue;
        // A layout can report count != 0 with a null `ranges`; dereferencing that crashed at launch.
        if (p.descriptor_table.ranges == nullptr || p.descriptor_table.count == 0) continue;
        const uint32_t nrange = LayoutParamRangeCount(p);
        logger::trace("U0WALK param=%u type=%d count=%u", i, (int)p.type, nrange);
        for (uint32_t j = 0; j < nrange; ++j) {
            descriptor_range r;
            if (!LayoutParamRange(p, j, r)) break;
            logger::trace("U0WALK   range %u/%u type=%d reg=%u space=%u count=%u binding=%u vis=%u", j, nrange,
                (int)r.type, r.dx_register_index, r.dx_register_space, r.count, r.binding, (unsigned)r.visibility);
            const bool csv =
                (r.visibility == shader_stage::all) || ((uint32_t)(r.visibility & shader_stage::compute) != 0);
            const uint32_t rc = r.count;
            const bool unbounded = (rc == 0xFFFFFFFFu);
            // Covers `reg` if the range starts at or below it and extends past it; unbounded runs to heap end.
            const bool covers = (r.dx_register_index <= reg) && (unbounded || reg < r.dx_register_index + rc);
            if (r.type == descriptor_type::unordered_access_view && r.dx_register_space == 0 && csv && covers) {
                out->param = (uint8_t)i;
                out->offset = r.binding + (reg - (uint32_t)r.dx_register_index);
                return true;
            }
        }
    }
    return false;
}

int LayoutRegistry::RootOf(const std::unordered_map<uint64_t, int>& m, uint64_t layout)
{
    auto it = m.find(layout);
    return it == m.end() ? -1 : it->second;
}

void LayoutRegistry::BoneRoots(uint64_t layout, int* b4, int* b3)
{
    std::lock_guard l(mutex_);
    auto a = b4_root_.find(layout);
    if (a != b4_root_.end()) *b4 = a->second;
    auto b = b3_root_.find(layout);
    if (b != b3_root_.end()) *b3 = b->second;
}

int LayoutRegistry::ObjectCbRoot(uint64_t layout)
{
    std::lock_guard l(mutex_);
    return RootOf(b2_root_, layout);
}

int LayoutRegistry::CsCb0Root(uint64_t layout)
{
    std::lock_guard l(mutex_);
    return RootOf(b0cs_root_, layout);
}

int LayoutRegistry::CsU0Root(uint64_t layout)
{
    std::lock_guard l(mutex_);
    return RootOf(u0cs_root_, layout);
}

bool LayoutRegistry::U0Table(uint64_t layout, U0CsTable* out)
{
    std::lock_guard l(mutex_);
    auto it = u0cs_table_.find(layout);
    if (it == u0cs_table_.end()) return false;
    *out = it->second;
    return true;
}

bool LayoutRegistry::CsCbvRootsOf(uint64_t layout, CsCbvRoots* out)
{
    std::lock_guard l(mutex_);
    auto it = cs_cbv_roots_.find(layout);
    if (it == cs_cbv_roots_.end()) return false;
    *out = it->second;
    return true;
}

void LayoutRegistry::TexScanLens(uint64_t layout, uint32_t (&out)[64])
{
    for (uint32_t& n : out) n = kTexScanMin;
    if (!layout) return;
    std::lock_guard l(tables_mutex_);
    auto it = tables_.find(layout);
    if (it == tables_.end()) return;
    for (const auto& t : it->second) {
        if (t.param >= 64 || !t.is_srv) continue;
        if (t.count == 0)
            out[t.param] = kTexScanMax;
        else if (t.count > kTexScanMin)
            out[t.param] = t.count < kTexScanMax ? t.count : kTexScanMax;
    }
}

void LayoutRegistry::Register(uint32_t count, const pipeline_layout_param* params, uint64_t layout)
{
    int b4 = FindVsCbvRoot(count, params, 4); // current skeleton  (vs-cb4)
    int b3 = FindVsCbvRoot(count, params, 3); // previous skeleton (vs-cb3)
    int b2 = FindVsCbvRoot(count, params, 2); // the drawn object  (vs-cb2)
    if (b4 >= 0) {
        std::lock_guard l(mutex_);
        auto prev = b4_root_.find(layout);
        if (prev != b4_root_.end() && prev->second != b4)
            logger::write("LAYOUT RECYCLE(live) handle=%llx b4root %d -> %d (destroy was missed)",
                (unsigned long long)layout, prev->second, b4);
        auto dead = dead_.find(layout);
        if (dead != dead_.end()) {
            ++recycled_;
            if (dead->second != b4)
                logger::write("LAYOUT RECYCLE handle=%llx b4root %d -> %d  (recycles so far=%u)",
                    (unsigned long long)layout, dead->second, b4, recycled_);
            dead_.erase(dead);
        }
        b4_root_[layout] = b4;
        b3_root_[layout] = b3;
        b2_root_[layout] = b2;
    }
    int b0cs = FindCsCbvRoot(count, params, 0); // shapekey Loader/Multiplier read cb0 @ b0 (compute-visible)
    if (b0cs >= 0) {
        std::lock_guard l(mutex_);
        b0cs_root_[layout] = b0cs;
    }
    {
        CsCbvRoots r;
        bool any = false;
        for (int reg = 0; reg < 16; ++reg) {
            r.reg[reg] = FindCsCbvRoot(count, params, reg);
            if (r.reg[reg] >= 0) any = true;
        }
        if (any) {
            std::lock_guard l(mutex_);
            cs_cbv_roots_[layout] = r;
        }
    }
    int u0cs = FindCsUavRoot(count, params, 0); // shapekey offsets buffer @ u0 -- the checktextureoverride target
    if (u0cs >= 0) {
        std::lock_guard l(mutex_);
        u0cs_root_[layout] = u0cs;
    }
    {
        U0CsTable u0t{}; // ...and the table form, which is how it is actually bound
        if (FindCsUavTable(count, params, 0, &u0t)) {
            std::lock_guard l(mutex_);
            u0cs_table_[layout] = u0t;
        }
    }
    {
        struct CsUavTables {
            U0CsTable t[8];
            bool has[8];
        };
        CsUavTables ut{};
        for (uint32_t reg = 0; reg < 8; ++reg) {
            U0CsTable e{};
            if (FindCsUavTable(count, params, reg, &e)) {
                ut.t[reg] = e;
                ut.has[reg] = true;
            }
        }
        {
            char buf[128];
            int n = 0;
            for (uint32_t reg = 0; reg < 8; ++reg)
                if (ut.has[reg])
                    n += sprintf_s(
                        buf + n, sizeof(buf) - n, " u%u=p%u[%u]", reg, (unsigned)ut.t[reg].param, ut.t[reg].offset);
            if (n) logger::write("CSUAVREGS layout=%llx --%s", (unsigned long long)layout, buf);
        }
    }

    {
        std::vector<LayoutTableParam> tbls;
        for (uint32_t i = 0; i < count && i < 64; ++i) {
            const auto& p = params[i];
            if (LayoutParamIsRootDescriptor(p)) continue;
            const uint32_t nrange = LayoutParamRangeCount(p);
            if (nrange == 0) continue;
            descriptor_range r0;
            if (!LayoutParamRange(p, 0, r0)) continue;
            descriptor_type dt = r0.type;
            uint8_t kind =
                (dt == descriptor_type::sampler || dt == descriptor_type::sampler_with_resource_view) ? 0 : 1;
            uint8_t is_srv = (dt == descriptor_type::shader_resource_view) ? 1 : 0;
            // total declared descriptors across the table's ranges; UINT32_MAX = unbounded.
            uint32_t tcount = 0;
            bool unbounded = false;
            for (uint32_t j = 0; j < nrange; ++j) {
                descriptor_range rj;
                if (!LayoutParamRange(p, j, rj)) break;
                uint32_t c = rj.count;
                if (c == 0xFFFFFFFFu) {
                    unbounded = true;
                    break;
                }
                tcount += c;
            }
            tbls.push_back({ (uint8_t)i, kind, is_srv, unbounded ? 0u : tcount, (uint32_t)r0.dx_register_index });
        }
        if (!tbls.empty()) {
            std::lock_guard l(tables_mutex_);
            tables_[layout] = std::move(tbls);
        }
    }

    {
        LayoutRegMap m;
        auto put = [](RegBind* arr, size_t cap, uint32_t reg, uint8_t param, uint32_t off, bool tbl) {
            if (reg >= cap) return; // beyond what we track; not an error
            if (arr[reg].param != 0xFF) return; // FIRST declaration wins -- a later duplicate
            // range would otherwise silently reassign it
            arr[reg].param = param;
            arr[reg].offset = off;
            arr[reg].is_table = tbl;
        };
        for (uint32_t i = 0; i < count && i < 64; ++i) {
            const auto& p = params[i];

            if (LayoutParamIsRootDescriptor(p)) {
                descriptor_range r;
                if (!LayoutParamRange(p, 0, r)) continue;
                if (r.dx_register_space != 0) continue; // space0 only, as everywhere else in this port
                const bool cs =
                    (r.visibility == shader_stage::all) || ((uint32_t)(r.visibility & shader_stage::compute) != 0);
                const bool gfx =
                    (r.visibility == shader_stage::all) || ((uint32_t)(r.visibility & ~shader_stage::compute) != 0);
                const bool vs =
                    (r.visibility == shader_stage::all) || ((uint32_t)(r.visibility & shader_stage::vertex) != 0);
                const bool ps =
                    (r.visibility == shader_stage::all) || ((uint32_t)(r.visibility & shader_stage::pixel) != 0);
                const uint32_t reg = (uint32_t)r.dx_register_index;
                if (r.type == descriptor_type::constant_buffer) {
                    if (cs) put(m.b_cs, 16, reg, (uint8_t)i, 0, false);
                    if (gfx) put(m.b_gfx, 16, reg, (uint8_t)i, 0, false);
                    if (vs) put(m.b_vs, 16, reg, (uint8_t)i, 0, false);
                    if (ps) put(m.b_ps, 16, reg, (uint8_t)i, 0, false);
                } else if (r.type == descriptor_type::shader_resource_view) {
                    if (cs) put(m.t_cs, 128, reg, (uint8_t)i, 0, false);
                    if (gfx) put(m.t_gfx, 128, reg, (uint8_t)i, 0, false);
                    if (vs) put(m.t_vs, 128, reg, (uint8_t)i, 0, false);
                    if (ps) put(m.t_ps, 128, reg, (uint8_t)i, 0, false);
                } else if (r.type == descriptor_type::unordered_access_view) {
                    if (cs) put(m.u_cs, 16, reg, (uint8_t)i, 0, false);
                    if (gfx) put(m.u_gfx, 16, reg, (uint8_t)i, 0, false);
                }
                continue;
            }

            const uint32_t nrange = LayoutParamRangeCount(p);
            if (nrange == 0) continue;

            for (uint32_t j = 0; j < nrange; ++j) {
                descriptor_range r;
                if (!LayoutParamRange(p, j, r)) break;
                const uint32_t rcount = r.count;
                if (rcount == 0xFFFFFFFFu || rcount > 4096u) continue; // unbounded/bindless: cannot enumerate
                if (r.dx_register_space != 0) continue;
                const bool cs =
                    (r.visibility == shader_stage::all) || ((uint32_t)(r.visibility & shader_stage::compute) != 0);
                const bool gfx =
                    (r.visibility == shader_stage::all) || ((uint32_t)(r.visibility & ~shader_stage::compute) != 0);
                const bool vs =
                    (r.visibility == shader_stage::all) || ((uint32_t)(r.visibility & shader_stage::vertex) != 0);
                const bool ps =
                    (r.visibility == shader_stage::all) || ((uint32_t)(r.visibility & shader_stage::pixel) != 0);
                for (uint32_t k = 0; k < rcount; ++k) {
                    const uint32_t reg = (uint32_t)r.dx_register_index + k;
                    const uint32_t off = r.binding + k;
                    if (r.type == descriptor_type::constant_buffer) {
                        if (cs) put(m.b_cs, 16, reg, (uint8_t)i, off, true);
                        if (gfx) put(m.b_gfx, 16, reg, (uint8_t)i, off, true);
                        if (vs) put(m.b_vs, 16, reg, (uint8_t)i, off, true);
                        if (ps) put(m.b_ps, 16, reg, (uint8_t)i, off, true);
                    } else if (r.type == descriptor_type::shader_resource_view) {
                        if (cs) put(m.t_cs, 128, reg, (uint8_t)i, off, true);
                        if (gfx) put(m.t_gfx, 128, reg, (uint8_t)i, off, true);
                        if (vs) put(m.t_vs, 128, reg, (uint8_t)i, off, true);
                        if (ps) put(m.t_ps, 128, reg, (uint8_t)i, off, true);
                    } else if (r.type == descriptor_type::unordered_access_view) {
                        if (cs) put(m.u_cs, 16, reg, (uint8_t)i, off, true);
                        if (gfx) put(m.u_gfx, 16, reg, (uint8_t)i, off, true);
                    }
                }
            }
        }
        m.built = true;
        {
            std::lock_guard l(regmap_mutex_);
            regmap_[layout] = m;
        }
    }

    {
        RegBind rb4{};
        RegBind rb3{};
        RegBind rb2{};
        {
            std::lock_guard l(regmap_mutex_);
            auto it = regmap_.find(layout);
            if (it != regmap_.end()) {
                rb4 = it->second.b_gfx[4];
                rb3 = it->second.b_gfx[3];
                rb2 = it->second.b_gfx[2];
            }
        }
        const bool have4 = (rb4.param != 0xFF);
        const bool tbl4 = have4 && rb4.is_table;
        if (b4 < 0 && have4) {
            static std::atomic<int> s_n{ 0 };
            const int n = s_n.fetch_add(1);
            if (!tbl4) {
                std::lock_guard l(mutex_);
                b4_root_[layout] = (int)rb4.param;
                if (rb3.param != 0xFF && !rb3.is_table)
                    b3_root_[layout] = (int)rb3.param;
                else
                    b3_root_[layout] = -1;
                b2_root_[layout] = (rb2.param != 0xFF && !rb2.is_table) ? (int)rb2.param : -1;
                if (n < 12)
                    logger::write("B4MAP #%d layout=%llx FindVsCbvRoot=-1 but regmap b_gfx[4]=param %u "
                                  "(root descriptor) -- adopting it; b3=%d%s",
                        n, (unsigned long long)layout, (unsigned)rb4.param,
                        (rb3.param != 0xFF && !rb3.is_table) ? (int)rb3.param : -1,
                        (rb3.param != 0xFF && rb3.is_table) ? " (b3 is a TABLE, dropped)" : "");
            } else if (n < 12) {
                logger::write("B4MAP #%d layout=%llx FindVsCbvRoot=-1 and regmap b_gfx[4] is a TABLE "
                              "(param %u off %u) -- NOT adopted, b4root stays -1. The skinning CB moved "
                              "out of the root; binding it needs the table path, not this index.",
                    n, (unsigned long long)layout, (unsigned)rb4.param, rb4.offset);
            }
        } else if (b4 < 0 && !have4) {
            static std::atomic<int> s_m{ 0 };
            const int m2 = s_m.fetch_add(1);
            if (m2 < 12)
                logger::write("B4MAP #%d layout=%llx params=%u: NO b4 at all -- neither FindVsCbvRoot nor "
                              "regmap b_gfx[4] declares register b4 in space0. Either this layout is not a "
                              "skinned-character one, or the skinning CB is no longer at b4.",
                    m2, (unsigned long long)layout, count);
        }
    }

    {
        bool has_cbv = false;
        for (uint32_t i = 0; i < count && !has_cbv; ++i) {
            const auto& p = params[i];
            const uint32_t nr = LayoutParamRangeCount(p);
            for (uint32_t j = 0; j < nr; ++j) {
                descriptor_range r;
                if (!LayoutParamRange(p, j, r)) break;
                if (r.type == descriptor_type::constant_buffer) {
                    has_cbv = true;
                    break;
                }
            }
        }
        static ProbeBudget s_laydump;
        if (count >= 4 && s_laydump.take(40)) {
            std::string s;
            char buf[64];
            for (uint32_t i = 0; i < count; ++i) {
                const auto& p = params[i];
                const char* t = "?";
                int reg = -1;
                int space = -1;
                int typ = -1;
                descriptor_range r0{};
                descriptor_range_flags rf = descriptor_range_flags::none;
                const bool have_r0 = LayoutParamRange(p, 0, r0, nullptr, &rf);
                {
                    const uint32_t nrf = LayoutParamRangeCount(p);
                    for (uint32_t j = 1; j < nrf; ++j) {
                        descriptor_range rj;
                        descriptor_range_flags fj = descriptor_range_flags::none;
                        if (!LayoutParamRange(p, j, rj, nullptr, &fj)) break;
                        rf = (descriptor_range_flags)((uint32_t)rf | (uint32_t)fj);
                    }
                }
                if (p.type == pipeline_layout_param_type::push_constants) {
                    t = "const";
                    reg = (int)p.push_constants.dx_register_index;
                } else if (have_r0 && LayoutParamIsRootDescriptor(p)) {
                    t = "root";
                    reg = (int)r0.dx_register_index;
                    space = (int)r0.dx_register_space;
                    typ = (int)r0.type;
                } else if (have_r0) {
                    t = "table";
                    reg = (int)r0.dx_register_index;
                    space = (int)r0.dx_register_space;
                    typ = (int)r0.type;
                }
                char fb[8];
                _snprintf_s(buf, sizeof(buf), "[%u:%s r%d s%d ty%d f%s]", i, t, reg, space, typ, RangeFlagsStr(rf, fb));
                s += buf;
            }
            logger::write("LAYOUT %llx b4root=%d has_cbv=%d params=%u %s", layout, b4, (int)has_cbv, count, s.c_str());
        }
    }
}

void LayoutRegistry::Forget(uint64_t layout)
{
    {
        std::lock_guard l(mutex_);
        auto it = b4_root_.find(layout);
        if (it != b4_root_.end()) dead_[layout] = it->second;
        b4_root_.erase(layout);
        b3_root_.erase(layout);
        b2_root_.erase(layout);
        b0cs_root_.erase(layout);
        cs_cbv_roots_.erase(layout);
        u0cs_root_.erase(layout);
        u0cs_table_.erase(layout);
    }
    {
        std::lock_guard l(tables_mutex_);
        tables_.erase(layout);
    }
    {
        std::lock_guard l(regmap_mutex_);
        regmap_.erase(layout);
    }
}

void on_init_pipeline_layout(device*, uint32_t count, const pipeline_layout_param* params, pipeline_layout layout)
{
    g_layouts.Register(count, params, layout.handle);
}

void on_destroy_pipeline_layout(device*, pipeline_layout layout)
{
    g_layouts.Forget(layout.handle);
    g_command_lists.ForEach([&](CmdBlock& b) {
        if (b.cmp_layout == layout.handle) b.cmp_layout = 0;
    });
}
