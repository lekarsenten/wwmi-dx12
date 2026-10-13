// Running THEIR ini sections with our draw-path callbacks installed. See port_section.h.
#include "../addon_common.h"
#include "port_section.h"
#include "../instrumentation/chain_trace.h" // g_chain_trace -- PortRunSection writes the K8 trace as it runs

std::vector<CsTrig>                        g_cs_trigs; // dispatch-bound
std::mutex                                 g_cs_trigs_mtx;
std::shared_ptr<const std::vector<CsTrig>> g_cs_trigs_snap;

bool SectionReadsPipelineState(const wchar_t* fq)
{
    return PortApi::SectionReadsShaderConditions(fq);
}

std::wstring PortComponentSection(int mod_id, int comp_index, const std::wstring& tail, const std::wstring& ns)
{
    if (ns.empty() || tail.empty()) return std::wstring();
    static std::mutex                                  s_mtx;
    static std::map<std::pair<int, int>, std::wstring> s_cache;
    static unsigned                                    s_gen = 0;
    PortApi::EngineScope                               engine; // outermost, as everywhere: ResolveSection takes it again inside
    std::lock_guard<std::mutex>                        lk(s_mtx);
    const unsigned                                     gen = PortApi::LoadGeneration();
    if (s_gen != gen) {
        s_gen = gen;
        s_cache.clear();
    }
    auto key = std::make_pair(mod_id, comp_index);
    auto it = s_cache.find(key);
    if (it == s_cache.end()) {
        wchar_t buf[512] = {};
        PortApi::ResolveSection(ns.c_str(), nullptr, tail.c_str(), buf, std::size(buf));
        it = s_cache.emplace(key, std::wstring(buf)).first;
    }
    return it->second;
}

bool PortRunSection(const std::wstring& fq_section, PortHooks& hk,
    uint32_t index_count, uint32_t first_index, uint32_t first_instance, uint64_t pso,
    bool& skip_orig)
{
#define PORT_STAGE(n, msg) \
    do { \
        static std::atomic<bool> _s{ false }; \
        if (!_s.exchange(true)) logger::write("PORTDRAW stage %d: %s", (n), (msg)); \
    } while (0)
    PORT_STAGE(1, "entered PortRunSection");

    struct Ctx {
        std::wstring             fq; // the section being executed, for the K8 trace
        PortHooks*               hk;
        const PortApi::DrawInfo* di = nullptr;      // srv_gen_at_entry, set by RunSectionByName under its lock
        bool                     vb0_unmet = false; // positions or indices bound from a [Resource*] with no DX12 object yet
        bool                     ib_unmet = false;
        std::wstring             unmet_res;      // the last such resource, for the log
        unsigned                 suppressed = 0; // draws not issued because of it
        std::vector<uint32_t>    this_pre;       // ps-tN write generations when a `this =` began; empty = none pending
        void                     OpenThis()
        {
            const MigotoPort::PipeState& st = MigotoPort::State();
            const int                    si = MigotoPort::StageIndex('p');
            this_pre.assign(st.srv_gen[si], st.srv_gen[si] + MigotoPort::kMaxSRV);
        }
        void CloseThis()
        {
            if (this_pre.empty()) return;
            const MigotoPort::PipeState& st = MigotoPort::State();
            const int                    si = MigotoPort::StageIndex('p');
            hk->this_gen.resize(MigotoPort::kMaxSRV, 0);
            for (unsigned r = 0; r < MigotoPort::kMaxSRV; ++r)
                if (st.srv_gen[si][r] != this_pre[r]) hk->this_gen[r] = st.srv_gen[si][r];
            this_pre.clear();
        }
    } ctx{ fq_section, &hk };

    if (g_chain_trace.IsOpen() || logger::enabled(1))
        PortApi::SetCommandObserver([](const wchar_t* line, void* user) {
            Ctx* c = (Ctx*)user;
            g_chain_trace.Line("RunCommandList", c->fq.c_str(), line);
            if (logger::enabled(1)) {
                static std::atomic<int> budget{ 400 };
                if (budget.fetch_sub(1) > 0) {
                    std::wstring cns;
                    if (line && line[0] == L'[') {
                        const wchar_t* close = wcschr(line, L']');
                        if (close) {
                            std::wstring sec(line + 1, close);
                            const size_t a = sec.find(L'\\');
                            const size_t b = sec.rfind(L'\\');
                            if (a != std::wstring::npos && b != std::wstring::npos && b > a)
                                cns = sec.substr(a + 1, b - a - 1);
                        }
                    }
                    logger::write("CHAIN  %-58ls | mod_enabled=%g object_detected=%g state_id=%g | "
                                  "vg_offset=%g vg_count=%g scale=%g",
                        line ? line : L"(null)",
                        cns.empty() ? -9.f : PortApi::GetVarNs(cns.c_str(), L"mod_enabled", -9.f),
                        cns.empty() ? -9.f : PortApi::GetVarNs(cns.c_str(), L"object_detected", -9.f),
                        cns.empty() ? -9.f : PortApi::GetVarNs(cns.c_str(), L"state_id", -9.f),
                        PortApi::GetVarNs(L"WWMIv1", L"vg_offset", -1.f),
                        PortApi::GetVarNs(L"WWMIv1", L"vg_count", -1.f),
                        PortApi::GetVarNs(L"WWMIv1", L"custom_mesh_scale", -1.f));
                }
            }
        },
            &ctx);

    PortApi::SetTypedObserver([](const PortApi::ObservedCommand& oc, void* user) {
        Ctx* c = (Ctx*)user;
        c->CloseThis();
        if (oc.kind == PortApi::ObservedCommand::Kind::ResourceCopy && oc.dst_is_this) c->OpenThis();
        if (oc.post) return;

        if (oc.kind == PortApi::ObservedCommand::Kind::RunList) {
            if (!oc.run_target) return;
            if (wcsncmp(oc.run_target, L"customshader", 12) == 0) {
                if (c->hk->cs_enter) c->hk->cs_enter(std::wstring(), std::wstring(oc.run_target));
            } else if (c->hk->run_cmd && wcsstr(oc.run_target, L"rabbitfx")) {
                c->hk->run_cmd(std::wstring(), std::wstring(oc.run_target));
            }
            return;
        }
        if (oc.kind != PortApi::ObservedCommand::Kind::ResourceCopy) return;

        if (oc.dst_is_srv) return; // `ps-tN = <resource|null>`: not a geometry bind, nothing applies it
        if (oc.dst_is_vb || oc.dst_is_ib) {
            const bool unmet = oc.src_is_custom && oc.src_resource && !PortApi::CustomResourceDx12(oc.src_custom);
            if (unmet) {
                MigotoPort::ShimUnmet(oc.dst_is_ib ? L"ib = <resource with no DX12 object>"
                                                   : L"vbN = <resource with no DX12 object>",
                    oc.src_resource);
                c->unmet_res = oc.src_resource;
            }
            if (oc.dst_is_ib)
                c->ib_unmet = unmet;
            else if (oc.dst_slot == 0)
                c->vb0_unmet = unmet;
        }
        // A hand-over into a [Resource*] section -- how RabbitFX passes its GlowMap and base maps.
        if (oc.dst_is_custom && oc.dst_resource && c->hk->resource_ref && wcsstr(oc.dst_resource, L"rabbitfx"))
            c->hk->resource_ref(std::wstring(oc.dst_resource),
                oc.src_resource ? std::wstring(oc.src_resource) : std::wstring());
    },
        &ctx);

    PortApi::SetDrawCallback([](const PortApi::ModDraw& d, void* user) {
        Ctx* c = (Ctx*)user;
        c->CloseThis();
        if (c->di) c->hk->srv_gen0 = c->di->srv_gen_at_entry;
        if (c->vb0_unmet || c->ib_unmet)
            ++c->suppressed;
        else if (c->hk->drawindexed)
            c->hk->drawindexed(d.index_count, d.start_index, (uint32_t)d.base_vertex);
    },
        &ctx);

    PortApi::DrawInfo di;
    di.index_count = index_count;
    di.first_index = first_index;
    di.first_instance = first_instance;
    di.indexed = true;
    di.pso = pso;

    PORT_STAGE(2, "callbacks installed, about to run their section");
    CrashHandler::ScopedContext crumb(fq_section.c_str());
    ctx.di = &di;
    const bool ran = PortApi::RunSectionThenPost(fq_section.c_str(), &di);
    PORT_STAGE(3, "their section returned");

    PortApi::SetCommandObserver(nullptr, nullptr);
    g_chain_trace.SyncGlobalObserver();
    PortApi::SetTypedObserver(nullptr, nullptr);
    PortApi::SetDrawCallback(nullptr, nullptr);

    // `handling = skip` comes back on the draw info, not as an event.
    if (ctx.suppressed && logger::enabled(1) && LogBudget("GEOM-UNMET", fq_section, 1))
        logger::write("GEOM-UNMET '%ls': %u draw(s) withheld -- '%ls' has no DX12 object yet (a shapekey pass not run?); "
                      "the game's own draw is kept until it does",
            fq_section.c_str(), ctx.suppressed, ctx.unmet_res.c_str());
    if (di.skip && !ctx.suppressed) {
        skip_orig = true;
        if (hk.handling_skip) hk.handling_skip();
    }
    PORT_STAGE(4, "PortRunSection complete");
#undef PORT_STAGE
    return ran;
}

bool LogBudget(const char* tag, const std::wstring& ns, int limit)
{
    static std::mutex                            m;
    static std::unordered_map<std::wstring, int> seen;
    std::wstring                                 k;
    for (const char* t = tag; *t; ++t) k += (wchar_t)(unsigned char)*t; // tags are ASCII literals
    k += L'|';
    for (wchar_t c : ns) k += (wchar_t)towlower(c);
    std::lock_guard<std::mutex> l(m);
    int&                        n = seen[k];
    if (n >= limit) return false;
    ++n;
    return true;
}
