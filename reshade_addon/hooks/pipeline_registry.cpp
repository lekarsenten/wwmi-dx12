// The pipeline registry, shader replacement and the pipeline-creation hooks. See pipeline_registry.h.
#include "../addon_common.h"
#include "pipeline_registry.h"
#include "command_list_registry.h"
#include "../mods/shader_alias.h" // AliasShaderHashAlways -- shader identity is registered at creation
#include "../hud/uid_hide.h"
#include "../core/dxil_cb.h" // DxilCbuffers -- the VS's declared cbuffer sizes, from the DXIL metadata
#include "../addon/addon.h"
#include "../core/fnv.h"
#include "hooks_toggles.h"
#include "../instrumentation/diag_gate.h"
#include "../instrumentation/shader_capture.h"
#include "../rabbitfx/pso_clone.h"
#include "../rabbitfx/rfx_patch.h"
#include "../migoto/dx12_input_layout.h"
#include "../migoto/port_api.h"
#include "../mods/wwmi_runtime.h"

PipelineRegistry g_pipelines;

namespace {
struct HookTimer {
    std::atomic<uint64_t>& n;
    std::atomic<uint64_t>& us;
    std::atomic<uint64_t>* max_us;
    LARGE_INTEGER t0;
    HookTimer(std::atomic<uint64_t>& n_, std::atomic<uint64_t>& us_, std::atomic<uint64_t>* max_)
        : n(n_),
          us(us_),
          max_us(max_)
    {
        QueryPerformanceCounter(&t0);
    }
    ~HookTimer()
    {
        static const LONGLONG freq = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return f.QuadPart;
        }();
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        const uint64_t d = (uint64_t)((t1.QuadPart - t0.QuadPart) * 1000000 / freq);
        n.fetch_add(1, std::memory_order_relaxed);
        us.fetch_add(d, std::memory_order_relaxed);
        if (max_us) {
            uint64_t m = max_us->load(std::memory_order_relaxed);
            while (d > m && !max_us->compare_exchange_weak(m, d, std::memory_order_relaxed)) {
            }
        }
    }
};
}

// Open addressing; a slot is made once per handle (the runtime recycles handles, so the key count is bounded by
// the peak of live PSOs) under add_, and the table doubles at half full. A replaced table is kept, never freed,
// so a lookup still inside it stays safe. A hash is only ever written non-zero; a destroyed PSO's are zeroed.
uint32_t PipelineRegistry::Hash(uint64_t key, uint32_t cap)
{
    return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1);
}

PipelineRegistry::Slot* PipelineRegistry::FindIn(const Tab* t, uint64_t key)
{
    for (uint32_t i = Hash(key, t->cap);; i = (i + 1) & (t->cap - 1)) {
        const uint64_t k = t->e[i].key.load(std::memory_order_acquire);
        if (k == key) return &t->e[i];
        if (k == 0) return nullptr;
    }
}

PipelineRegistry::Slot* PipelineRegistry::PutIn(Tab* t, uint64_t key) // under add_; the key is published last
{
    uint32_t i = Hash(key, t->cap);
    while (t->e[i].key.load(std::memory_order_relaxed)) i = (i + 1) & (t->cap - 1);
    return &t->e[i];
}

PipelineRegistry::Slot* PipelineRegistry::SlotOf(uint64_t key, bool create)
{
    if (!key) return nullptr;
    if (Slot* s = FindIn(tab_.load(std::memory_order_acquire), key)) return s;
    if (!create) return nullptr;
    std::lock_guard lock(add_);
    Tab* t = tab_.load(std::memory_order_relaxed);
    if (Slot* s = FindIn(t, key)) return s;
    if ((used_ + 1) * 2 > t->cap) {
        Tab* g = new Tab{ t->cap * 2, new Slot[t->cap * 2] };
        for (uint32_t i = 0; i < t->cap; ++i) {
            const Slot& o = t->e[i];
            const uint64_t k = o.key.load(std::memory_order_relaxed);
            if (!k) continue;
            Slot* n = PutIn(g, k);
            n->vc.store(o.vc.load(std::memory_order_relaxed), std::memory_order_relaxed);
            n->ps.store(o.ps.load(std::memory_order_relaxed), std::memory_order_relaxed);
            n->gs.store(o.gs.load(std::memory_order_relaxed), std::memory_order_relaxed);
            n->dc.store(o.dc.load(std::memory_order_relaxed), std::memory_order_relaxed);
            n->psr.store(o.psr.load(std::memory_order_relaxed), std::memory_order_relaxed);
            n->text.store(o.text.load(std::memory_order_relaxed), std::memory_order_relaxed);
            n->uc.store(o.uc.load(std::memory_order_relaxed), std::memory_order_relaxed);
            n->ucz.store(o.ucz.load(std::memory_order_relaxed), std::memory_order_relaxed);
            n->chr.store(o.chr.load(std::memory_order_relaxed), std::memory_order_relaxed);
            n->chrg.store(o.chrg.load(std::memory_order_relaxed), std::memory_order_relaxed);
            n->key.store(k, std::memory_order_release);
        }
        tab_.store(g, std::memory_order_release);
        t = g;
    }
    Slot* s = PutIn(t, key);
    s->key.store(key, std::memory_order_release);
    ++used_;
    return s;
}

bool PipelineRegistry::CharacterPs(Slot* s, uint64_t ps)
{
    const uint32_t gen = char_ps_gen_.load(std::memory_order_acquire);
    if (s->chrg.load(std::memory_order_acquire) != gen) {
        bool chr;
        {
            std::lock_guard l(char_ps_mutex_);
            chr = char_ps_.count(ps) != 0;
        }
        s->chr.store(chr, std::memory_order_release);
        s->chrg.store(gen, std::memory_order_release);
    }
    return s->chr.load(std::memory_order_acquire);
}

void PipelineRegistry::BoundHashes(uint64_t pso, uint64_t* vs_or_cs, uint64_t* ps, uint64_t* gs)
{
    *vs_or_cs = *ps = *gs = 0;
    if (const Slot* s = SlotOf(pso, false)) {
        *vs_or_cs = s->vc.load(std::memory_order_acquire);
        *ps = s->ps.load(std::memory_order_acquire);
        *gs = s->gs.load(std::memory_order_acquire);
    }
}

uint64_t PipelineRegistry::DecensorVariant(device* dev, uint64_t pso)
{
    Slot* s = SlotOf(pso, false);
    if (!s) return 0;
    const uint64_t ps = s->ps.load(std::memory_order_acquire);
    if (!ps || !CharacterPs(s, ps)) return 0;
    const uint64_t v = s->dc.load(std::memory_order_acquire);
    if (v > 1) return v;
    uint64_t exp = 0;
    if (v == 0 && s->dc.compare_exchange_strong(exp, 1, std::memory_order_acq_rel)) g_rfx_patch.QueueDecensor(dev, pso);
    return 0;
}

void PipelineRegistry::SetDecensorVariant(uint64_t pso, uint64_t variant)
{
    if (Slot* s = SlotOf(pso, false)) s->dc.store(variant ? variant : 1, std::memory_order_release);
}

void PipelineRegistry::NoteCharacterDraw(uint64_t pso)
{
    Slot* s = SlotOf(pso, false);
    if (!s) return;
    const uint64_t ps = s->ps.load(std::memory_order_acquire);
    if (!ps || s->chr.load(std::memory_order_acquire)) return;
    std::lock_guard l(char_ps_mutex_);
    if (char_ps_.insert(ps).second) {
        char_ps_gen_.fetch_add(1, std::memory_order_acq_rel);
        if (g_debug)
            logger::write(
                "DECENSOR-CHAR ps=%016llx: drew a character -- decensored from its next bind (%zu character pixel shader(s))",
                (unsigned long long)ps, char_ps_.size());
    }
    s->chr.store(true, std::memory_order_release);
    s->chrg.store(char_ps_gen_.load(std::memory_order_acquire), std::memory_order_release);
}

uint64_t PipelineRegistry::UidVariant(device* dev, uint64_t pso, uint32_t target)
{
    Slot* s = SlotOf(pso, false);
    if (!s || !target || !s->text.load(std::memory_order_acquire)) return 0;
    uint32_t z = s->ucz.load(std::memory_order_acquire);
    if (z == target) {
        const uint64_t v = s->uc.load(std::memory_order_acquire);
        return v > 1 ? v : 0;
    }
    if (s->ucz.compare_exchange_strong(z, target, std::memory_order_acq_rel)) {
        s->uc.store(1, std::memory_order_release);
        g_rfx_patch.QueueUid(dev, pso, target, UidHide::Cuts(target));
    }
    return 0;
}

void PipelineRegistry::SetUidVariant(uint64_t pso, uint32_t target, uint64_t variant)
{
    if (Slot* s = SlotOf(pso, false))
        if (s->ucz.load(std::memory_order_acquire) == target)
            s->uc.store(variant ? variant : 1, std::memory_order_release);
}

uint64_t PipelineRegistry::PsReadMask(uint64_t pso)
{
    const Slot* s = SlotOf(pso, false);
    return s ? s->psr.load(std::memory_order_acquire) : ~0ull;
}

DxilSrvRegs PipelineRegistry::SrvRegs(uint64_t pso, char stage)
{
    std::lock_guard lock(regs_mutex_);
    auto& m = (stage == 'v') ? vs_regs_ : ps_regs_;
    auto it = m.find(pso);
    return it == m.end() ? DxilSrvRegs{} : it->second;
}

bool PipelineRegistry::VsCbufferBytes(uint64_t pso, unsigned reg, uint32_t& bytes)
{
    bytes = 0;
    if (reg >= 16) return false;
    std::lock_guard lock(regs_mutex_);
    auto it = vs_cbs_.find(pso);
    if (it == vs_cbs_.end()) return false;
    bytes = it->second[reg];
    return true;
}

bool PipelineRegistry::CullMode(uint64_t pso, cull_mode* out)
{
    std::lock_guard lock(regs_mutex_);
    auto it = cull_.find(pso);
    if (it == cull_.end()) return false;
    *out = it->second;
    return true;
}

PipelineHookStats PipelineRegistry::TakeStats()
{
    return { init_n_.exchange(0), init_us_.exchange(0), init_max_us_.exchange(0), destroy_n_.exchange(0),
        destroy_us_.exchange(0) };
}

static thread_local std::vector<std::vector<uint8_t>> s_replace_keep;
static bool LoadReplacementShader(shader_desc& desc)
{
    if (!desc.code || desc.code_size == 0) return false;
    uint64_t h = HashShader(desc.code, desc.code_size);
    wchar_t path[MAX_PATH];
    const wchar_t* which = nullptr;
    if (g_hooks_toggles.shader_replace) {
        swprintf_s(path, L"%s\\replace\\%016llx.cso", g_addon.cso_dir.c_str(), (unsigned long long)h);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) which = L"replace";
    }
    if (!which) return false;
    auto bytes = WwmiReadFile(path);
    if (bytes.empty()) return false;
    s_replace_keep.push_back(std::move(bytes));
    desc.code = s_replace_keep.back().data();
    desc.code_size = s_replace_keep.back().size();
    if (DiagOnce("SHADERREPLACE", (int)(h ^ (h >> 32))))
        logger::write("SHADERREPLACE %016llx -> %S\\%016llx.cso (%zu bytes)", (unsigned long long)h, which,
            (unsigned long long)h, s_replace_keep.back().size());
    return true;
}

bool on_create_pipeline(device*, pipeline_layout, uint32_t subobject_count, const pipeline_subobject* subobjects)
{
    if (!g_hooks_toggles.shader_replace) return false;
    bool replaced = false;
    for (uint32_t i = 0; i < subobject_count; ++i) {
        switch (subobjects[i].type) {
        case pipeline_subobject_type::vertex_shader:
        case pipeline_subobject_type::geometry_shader:
        case pipeline_subobject_type::pixel_shader:
        case pipeline_subobject_type::compute_shader:
            replaced |= LoadReplacementShader(*static_cast<shader_desc*>(subobjects[i].data));
            break;
        default: break;
        }
    }
    return replaced;
}

void PipelineRegistry::Register(
    device*, pipeline_layout layout, uint32_t subobject_count, const pipeline_subobject* subobjects, pipeline pipeline)
{
    HookTimer timed(init_n_, init_us_, &init_max_us_);
    uint64_t vs_hash = 0;
    uint64_t cs_hash = 0;
    uint64_t ps_hash = 0;
    uint64_t gs_hash = 0;
    const void* vs_code = nullptr;
    size_t vs_size = 0;
    const void* ps_code = nullptr;
    size_t ps_size = 0;
    const void* cs_code = nullptr;
    size_t cs_size = 0;
    for (uint32_t i = 0; i < subobject_count; ++i) {
        if (subobjects[i].type == pipeline_subobject_type::vertex_shader) {
            auto shader = static_cast<const shader_desc*>(subobjects[i].data);
            if (shader && shader->code_size > 0) {
                vs_hash = HashShader(shader->code, shader->code_size);
                vs_code = shader->code;
                vs_size = shader->code_size;
            }
        }
        if (subobjects[i].type == pipeline_subobject_type::compute_shader) {
            auto shader = static_cast<const shader_desc*>(subobjects[i].data);
            if (shader && shader->code_size > 0) {
                cs_hash = HashShader(shader->code, shader->code_size);
                cs_code = shader->code;
                cs_size = shader->code_size;
            }
        }
        if (subobjects[i].type == pipeline_subobject_type::pixel_shader) {
            auto shader = static_cast<const shader_desc*>(subobjects[i].data);
            if (shader && shader->code_size > 0) {
                ps_hash = HashShader(shader->code, shader->code_size);
                ps_code = shader->code;
                ps_size = shader->code_size;
            }
        }
        if (subobjects[i].type == pipeline_subobject_type::geometry_shader) {
            auto shader = static_cast<const shader_desc*>(subobjects[i].data);
            if (shader && shader->code_size > 0) gs_hash = HashShader(shader->code, shader->code_size);
        }
        if (subobjects[i].type == pipeline_subobject_type::input_layout) {
            auto elems = static_cast<const input_element*>(subobjects[i].data);
            if (elems && subobjects[i].count) {
                std::vector<MigotoPort::InputElement> in(subobjects[i].count);
                for (uint32_t k = 0; k < subobjects[i].count; ++k)
                    in[k] = { elems[k].semantic, elems[k].semantic_index, (unsigned)elems[k].format,
                        elems[k].buffer_binding, elems[k].offset, elems[k].instance_step_rate };
                MigotoPort::RecordInputLayout(pipeline.handle, in.data(), subobjects[i].count);
            }
        }
    }
    auto register_both = [](uint64_t h) {
        if (!h) return;
        PortApi::RegisterShaderHash(h);
        const uint64_t aliased = AliasShaderHashAlways(h);
        if (aliased != h) PortApi::RegisterShaderHash(aliased);
    };
    register_both(vs_hash);
    register_both(ps_hash);
    register_both(gs_hash);
    register_both(cs_hash);
    if (vs_code) g_shader_capture.Note(vs_hash, vs_code, vs_size, "vs");
    if (ps_code) g_shader_capture.Note(ps_hash, ps_code, ps_size, "ps");
    if (cs_code) g_shader_capture.Note(cs_hash, cs_code, cs_size, "cs");
    if (g_debug && vs_hash) { // log every distinct VS input layout (capped) -- incl. the depth VS
        static std::set<uint64_t> s_il_seen;
        bool fresh;
        {
            std::lock_guard lock(regs_mutex_);
            fresh = ProbeArmed() && (s_il_seen.size() < 80) && s_il_seen.insert(vs_hash).second;
        }
        if (fresh)
            for (uint32_t i = 0; i < subobject_count; ++i) {
                if (subobjects[i].type != pipeline_subobject_type::input_layout) continue;
                const auto* els = static_cast<const input_element*>(subobjects[i].data);
                std::string s;
                char b[96];
                for (uint32_t e = 0; e < subobjects[i].count; ++e) {
                    _snprintf_s(b, sizeof(b), "[%s%u slot%u off%u fmt%u]", els[e].semantic ? els[e].semantic : "?",
                        els[e].semantic_index, els[e].buffer_binding, els[e].offset, (uint32_t)els[e].format);
                    s += b;
                }
                logger::trace("INPUTLAYOUT vs=%016llx n=%u %s", vs_hash, subobjects[i].count, s.c_str());
            }
    }
    uint64_t ps_reads = ~0ull;
    {
        DxilSrvRegs pr;
        DxilSrvRegs vr;
        const bool pp = ps_code && DxilShaderSrvRegs(ps_code, ps_size, pr);
        if (pp) {
            ps_reads = 0;
            for (unsigned r = 0; r < 64; ++r)
                if (pr.reads(r)) ps_reads |= 1ull << r;
        }
        const bool vp = vs_code && DxilShaderSrvRegs(vs_code, vs_size, vr);
        std::lock_guard lock(regs_mutex_);
        if (pp)
            ps_regs_[pipeline.handle] = pr;
        else
            ps_regs_.erase(pipeline.handle);
        if (vp)
            vs_regs_[pipeline.handle] = vr;
        else
            vs_regs_.erase(pipeline.handle);
        if (g_debug && ps_code && !pp && DiagOnce("PSV0-NONE", (int)(ps_hash ^ (ps_hash >> 32))))
            logger::write(
                "PSV0: pixel shader %016llx carries no PSV0 part (%zu bytes) -- its register file is the root signature's",
                (unsigned long long)ps_hash, ps_size);
    }
    const bool text = ps_code && UidHide::IsTextShader(ps_code, ps_size);
    if (text)
        logger::write("UIDCUT: pso %llx draws UI text (ps=%016llx)", pipeline.handle, (unsigned long long)ps_hash);
    if (vs_hash || cs_hash || ps_hash || gs_hash)
        if (Slot* s = SlotOf(pipeline.handle, true)) {
            s->dc.store(0, std::memory_order_release);
            s->psr.store(ps_reads, std::memory_order_release);
            s->text.store(text, std::memory_order_release);
            s->uc.store(0, std::memory_order_release);
            s->ucz.store(0, std::memory_order_release);
            s->chr.store(false, std::memory_order_release);
            s->chrg.store(0, std::memory_order_release);
            if (vs_hash) s->vc.store(vs_hash, std::memory_order_release);
            if (cs_hash) s->vc.store(cs_hash, std::memory_order_release);
            if (ps_hash) s->ps.store(ps_hash, std::memory_order_release);
            if (gs_hash) s->gs.store(gs_hash, std::memory_order_release);
        }
    { // the cull mode, for A8 (a mod's outline shell is its front-culling pass)
        const rasterizer_desc* rs = nullptr;
        for (uint32_t i = 0; i < subobject_count; ++i)
            if (subobjects[i].type == pipeline_subobject_type::rasterizer_state && subobjects[i].count >= 1)
                rs = static_cast<const rasterizer_desc*>(subobjects[i].data);
        std::lock_guard lock(regs_mutex_);
        if (rs)
            cull_[pipeline.handle] = rs->cull_mode;
        else
            cull_.erase(pipeline.handle);
    }
    {
        std::vector<DxilCbuf> cbs;
        std::string why;
        const bool ok = vs_code && DxilCbuffers(vs_code, vs_size, cbs, &why);
        std::array<uint32_t, 16> by_reg{};
        if (ok)
            for (const DxilCbuf& c : cbs)
                if (c.space == 0 && c.reg < 16) by_reg[c.reg] = c.bytes;
        {
            std::lock_guard lock(regs_mutex_);
            if (ok)
                vs_cbs_[pipeline.handle] = by_reg;
            else
                vs_cbs_.erase(pipeline.handle);
        }
        if (g_debug && vs_code && DiagOnce("VSCBUF", (int)(vs_hash ^ (vs_hash >> 32)))) {
            if (!ok)
                logger::write(
                    "VSCBUF vs=%016llx: cbuffer declarations not read (%s)", (unsigned long long)vs_hash, why.c_str());
            else {
                std::string list;
                char b[96];
                for (const DxilCbuf& c : cbs) {
                    _snprintf_s(b, sizeof(b), " b%u=%u(%s)", c.reg, c.bytes, c.type.c_str());
                    list += b;
                }
                logger::trace("VSCBUF vs=%016llx%s", (unsigned long long)vs_hash, list.c_str());
            }
        }
    }

    if (vs_hash && ps_hash) g_pso_clones.Store(pipeline.handle, layout, subobject_count, subobjects);

    // Shader-replacement bytes (allocated in on_create_pipeline) are now safe to free -- the pipeline is built.
    if (!s_replace_keep.empty()) s_replace_keep.clear();
}

void PipelineRegistry::Forget(device* device, pipeline pipeline)
{
    HookTimer timed(destroy_n_, destroy_us_, nullptr);
    // The clone ring and the blend / glow variants are keyed by this handle, which the runtime recycles.
    g_pso_clones.Forget(device, pipeline.handle);
    {
        if (Slot* s = SlotOf(pipeline.handle, false)) {
            s->vc.store(0);
            s->ps.store(0);
            s->gs.store(0);
            s->dc.store(0);
            s->psr.store(~0ull);
            s->text.store(false);
            s->uc.store(0);
            s->ucz.store(0);
            s->chr.store(false);
            s->chrg.store(0);
        }
        std::lock_guard lock(regs_mutex_);
        ps_regs_.erase(pipeline.handle);
        vs_regs_.erase(pipeline.handle);
        vs_cbs_.erase(pipeline.handle);
        cull_.erase(pipeline.handle);
    }
    g_command_lists.ForgetPipeline(pipeline.handle);
}

void on_init_pipeline(device* device, pipeline_layout layout, uint32_t subobject_count,
    const pipeline_subobject* subobjects, pipeline pipeline)
{
    g_pipelines.Register(device, layout, subobject_count, subobjects, pipeline);
}

void on_destroy_pipeline(device* device, pipeline pipeline)
{
    g_pipelines.Forget(device, pipeline);
}
