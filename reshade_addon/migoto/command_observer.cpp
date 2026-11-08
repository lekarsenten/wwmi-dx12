#include "command_observer.h"
#include "selection_watch.h"
#include "port_api.h"
#include "port_api_internal.h" // LoadDeferredNow
#include "dx12_pipeline.h" // NoteTextureUse
#include "prof_core.h"
#include "CommandList.h" // the parsed command types the typed observer reads (theirs, unmodified)
#include "../core/log.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <typeinfo>

namespace MigotoPort {

CommandObserver& CommandObserverInstance()
{
    static CommandObserver* const instance = new CommandObserver();
    return *instance;
}

CommandObserver& g_command_observer = CommandObserverInstance();

namespace {
thread_local CommandObserverFn t_observer = nullptr;
thread_local void* t_observer_user = nullptr;
// Same thread-locality requirement, same reason: it points at the caller's stack frame.
thread_local TypedCommandObserverFn t_typed = nullptr;
thread_local void* t_typed_user = nullptr;
thread_local const wchar_t* t_cur_ini_line = nullptr;
thread_local std::vector<CommandListCommand*>* t_trace = nullptr;
thread_local uint64_t t_cmd_child = 0;
thread_local bool t_bound_loading = false; // PortApi::BoundLoading
thread_local CustomResource* t_this_loading = nullptr; // CommandObserver::ThisLoading

bool ClassifyCommand(const CommandListCommand* cmd, ::CommandListState* state, ObservedCmd& o)
{
    if (const RunExplicitCommandList* e = dynamic_cast<const RunExplicitCommandList*>(cmd)) {
        o.kind = ObservedCmd::Kind::RunList;
        o.run_explicit = (const void*)e->command_list_section;
    } else if (const RunLinkedCommandList* r = dynamic_cast<const RunLinkedCommandList*>(cmd)) {
        o.kind = ObservedCmd::Kind::RunList;
        if (r->link) o.run_target = r->link->ini_section.c_str();
    } else if (const ResourceCopyOperation* c = dynamic_cast<const ResourceCopyOperation*>(cmd)) {
        o.kind = ObservedCmd::Kind::ResourceCopy;
        o.dst_type = (int)c->dst.type;
        o.dst_shader = c->dst.shader_type;
        o.dst_slot = !c->dst.slot_expression ? c->dst.slot
            : state                          ? const_cast<ResourceCopyTarget&>(c->dst).ResolveSlot(state)
                                             : UINT_MAX;
        o.dst_custom = state ? (const void*)const_cast<ResourceCopyTarget&>(c->dst).GetCustomResource(state) : nullptr;
        o.src_type = (int)c->src.type;
        o.src_custom = state ? (const void*)const_cast<ResourceCopyTarget&>(c->src).GetCustomResource(state) : nullptr;
    } else {
        return false; // nothing the addon acts on
    }
    return true;
}

bool StaticTargets(const CommandListCommand* cmd)
{
    const ResourceCopyOperation* c = dynamic_cast<const ResourceCopyOperation*>(cmd);
    return !c ||
        (!c->dst.pool_dynamic_index_expression && !c->src.pool_dynamic_index_expression && !c->dst.slot_expression &&
            !c->src.slot_expression);
}

const char* TargetName(ResourceCopyTargetType t)
{
    switch (t) {
    case ResourceCopyTargetType::EMPTY: return "null";
    case ResourceCopyTargetType::CONSTANT_BUFFER: return "cb";
    case ResourceCopyTargetType::SHADER_RESOURCE: return "srv";
    case ResourceCopyTargetType::VERTEX_BUFFER: return "vb";
    case ResourceCopyTargetType::INDEX_BUFFER: return "ib";
    case ResourceCopyTargetType::RENDER_TARGET: return "rt";
    case ResourceCopyTargetType::DEPTH_STENCIL_TARGET: return "dsv";
    case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW: return "uav";
    case ResourceCopyTargetType::CUSTOM_RESOURCE: return "Resource";
    case ResourceCopyTargetType::INI_PARAMS: return "IniParams";
    case ResourceCopyTargetType::THIS_RESOURCE: return "this";
    default: return "other";
    }
}

std::string CostKindName(CommandListCommand* c)
{
    if (dynamic_cast<IfCommand*>(c)) return "if";
    if (dynamic_cast<RunExplicitCommandList*>(c)) return "run = CommandList";
    if (dynamic_cast<RunCustomShaderCommand*>(c)) return "run = CustomShader";
    if (dynamic_cast<ParamOverride*>(c)) return "IniParam = expr";
    if (dynamic_cast<VariableAssignment*>(c)) return "$var = expr";
    if (dynamic_cast<DrawCommand*>(c)) return "draw";
    if (dynamic_cast<CheckTextureOverrideCommand*>(c)) return "checktextureoverride";
    if (dynamic_cast<ClearViewCommand*>(c)) return "clear";
    if (const ResourceCopyOperation* rc = dynamic_cast<const ResourceCopyOperation*>(c)) {
        const int opts = (int)rc->options;
        const char* mode = (opts & (int)ResourceCopyOptions::REFERENCE) ? "ref"
            : (opts & (int)ResourceCopyOptions::COPY_DESC)              ? "copy_desc"
            : (opts & (int)ResourceCopyOptions::COPY_MASK)              ? "copy"
                                                                        : "assign";
        return std::string("resource ") + mode + " " + TargetName(rc->src.type) + " -> " + TargetName(rc->dst.type);
    }
    std::string n = typeid(*c).name();
    if (n.rfind("class ", 0) == 0) n.erase(0, 6);
    return n;
}

class ObservedCommand : public CommandListCommand {
public:
    std::shared_ptr<CommandListCommand> inner;
    ObservedCmd cached; // classified once here, when the targets are static
    bool relevant = false; // the typed observer hears of it at all
    bool cached_ok = false; // `cached` stands; else classified per run with the state
    const float* sel_var = nullptr;
    bool sel_always = false;
    bool sel_watched = false;
    unsigned sel_watch_gen = 0;
    CustomResource* sel_res = nullptr;
    CustomResource* file_src = nullptr; // a file texture this copies somewhere a draw does not sample it
    CustomResource* slot_src = nullptr; // ... or into a draw's slot
    ResourceCopyOperation* this_copy = nullptr;
    bool is_if = false;
    bool is_run = false;
    CommandObserver::CmdCost* cost = nullptr;
    explicit ObservedCommand(std::shared_ptr<CommandListCommand> c)
        : inner(std::move(c))
    {
        ini_line = inner->ini_line;
        CommandListCommand* ic = inner.get();
        cost = g_command_observer.CostKind(ic);
        is_if = dynamic_cast<IfCommand*>(ic) != nullptr;
        is_run = dynamic_cast<RunExplicitCommandList*>(ic) != nullptr;
        if (VariableAssignment* va = dynamic_cast<VariableAssignment*>(ic)) {
            if (va->var)
                sel_var = &va->var->fval;
            else
                sel_always = true;
        } else if (StoreCommand* sc = dynamic_cast<StoreCommand*>(ic)) {
            if (sc->var)
                sel_var = &sc->var->fval;
            else
                sel_always = true;
        } else if (dynamic_cast<PoolVariableOperation*>(ic) || dynamic_cast<CopyCommandListCommand*>(ic) ||
            dynamic_cast<SlotRangeCopyOperation*>(ic)) {
            sel_always = true;
        } else if (const ResourceCopyOperation* rc = dynamic_cast<const ResourceCopyOperation*>(ic)) {
            sel_always = rc->dst.type == ResourceCopyTargetType::POOL || rc->dst.custom_resource_pool ||
                rc->src.custom_resource_pool;
            if (!sel_always && rc->dst.type == ResourceCopyTargetType::CUSTOM_RESOURCE &&
                !rc->dst.pool_dynamic_index_expression)
                sel_res = const_cast<ResourceCopyTarget&>(rc->dst).GetCustomResource(nullptr);
            if (rc->dst.type == ResourceCopyTargetType::THIS_RESOURCE)
                this_copy = const_cast<ResourceCopyOperation*>(rc);
            const bool draw_slot =
                (rc->dst.type == ResourceCopyTargetType::SHADER_RESOURCE && rc->dst.shader_type != L'c') ||
                rc->dst.type == ResourceCopyTargetType::THIS_RESOURCE;
            if (rc->src.type == ResourceCopyTargetType::CUSTOM_RESOURCE && !rc->src.custom_resource_pool) {
                CustomResource* src = const_cast<ResourceCopyTarget&>(rc->src).GetCustomResource(nullptr);
                if (src && !src->filename.empty() && draw_slot)
                    slot_src = src;
                else if (src && !src->filename.empty())
                    file_src = src;
            }
        }
        relevant = ClassifyCommand(inner.get(), nullptr, cached);
        cached_ok = relevant && StaticTargets(inner.get());
        if (cached_ok) {
            if (const ResourceCopyOperation* rc = dynamic_cast<const ResourceCopyOperation*>(inner.get())) {
                cached.dst_custom = const_cast<ResourceCopyTarget&>(rc->dst).GetCustomResource(nullptr);
                cached.src_custom = const_cast<ResourceCopyTarget&>(rc->src).GetCustomResource(nullptr);
            }
        }
    }
    void run(CommandListState* state) override;
    bool optimise(HackerDevice* device) override { return inner->optimise(device); }
    bool noop(bool post, bool ignore_cto_pre, bool ignore_cto_post) override
    {
        return inner->noop(post, ignore_cto_pre, ignore_cto_post);
    }
};

bool RefFileBackedCopy(ResourceCopyOperation* c)
{
    if (!c || c->dst.type != ResourceCopyTargetType::CUSTOM_RESOURCE ||
        c->src.type != ResourceCopyTargetType::CUSTOM_RESOURCE)
        return false;
    if (c->dst.custom_resource_pool || c->src.custom_resource_pool) return false;
    const int opts = (int)c->options;
    const int plain = (int)ResourceCopyOptions::COPY | (int)ResourceCopyOptions::UNLESS_NULL;
    if (!(opts & (int)ResourceCopyOptions::COPY) || (opts & ~plain)) return false;
    CustomResource* src = c->src.GetCustomResource(nullptr, true);
    CustomResource* dst = c->dst.GetCustomResource(nullptr, true);
    if (!src || !dst || src->filename.empty()) return false;
    const UINT writes =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT;
    if (dst->bind_flags & writes) return false;
    D3D11_RESOURCE_MISC_FLAG misc = (D3D11_RESOURCE_MISC_FLAG)0;
    if (!src->AddFlags(c->dst.BindFlags(nullptr, &misc), misc, true)) return false; // their own `ref` check
    c->options = (ResourceCopyOptions)((opts & ~(int)ResourceCopyOptions::COPY) | (int)ResourceCopyOptions::REFERENCE);
    logger::write("PORT-REF: '%ls' copies a file-backed texture into a read-only resource -- made a reference",
        c->ini_line.c_str());
    return true;
}

size_t WrapList(CommandList* list)
{
    size_t n = 0;
    if (!list) return 0;
    for (auto& cmd : list->commands) {
        if (!cmd || dynamic_cast<ObservedCommand*>(cmd.get())) continue;
        RefFileBackedCopy(dynamic_cast<ResourceCopyOperation*>(cmd.get()));
        cmd = std::make_shared<ObservedCommand>(cmd);
        ++n;
    }
    return n;
}
} // namespace

void CommandObserver::SetObserver(CommandObserverFn fn, void* user)
{
    t_observer = fn;
    t_observer_user = user;
}

void CommandObserver::SetTypedObserver(TypedCommandObserverFn fn, void* user)
{
    t_typed = fn;
    t_typed_user = user;
}

void CommandObserver::Observe(const wchar_t* ini_line)
{
    if (t_observer && ini_line) t_observer(ini_line, t_observer_user);
}

const wchar_t* CommandObserver::CurrentIniLine() const
{
    return t_cur_ini_line;
}

bool CommandObserver::Active() const
{
    return t_observer != nullptr;
}

bool CommandObserver::Any() const
{
    return t_observer != nullptr || t_typed != nullptr;
}

std::vector<CommandListCommand*>* CommandObserver::SetTrace(std::vector<CommandListCommand*>* trace)
{
    std::vector<CommandListCommand*>* const prev = t_trace;
    t_trace = trace;
    return prev;
}

void CommandObserver::SetRunExplicitHook(bool (*fn)(CommandListCommand*, ::CommandListState*))
{
    run_explicit_hook_ = fn;
}

bool CommandObserver::RunExplicit(CommandListCommand* inner, ::CommandListState* state)
{
    return run_explicit_hook_ && run_explicit_hook_(inner, state);
}

void CommandObserver::Wrap(CommandList* list)
{
    wrapped_quiet_.fetch_add(WrapList(list), std::memory_order_relaxed);
}

void CommandObserver::WrapAll(std::vector<CommandList*>& lists)
{
    size_t n = 0;
    for (CommandList* list : lists) n += WrapList(list);
    logger::write("PORT-OBSERVE: %zu command(s) in %zu list(s) wrapped for the observer, plus %zu in override "
                  "lists",
        n, lists.size(), wrapped_quiet_.exchange(0, std::memory_order_relaxed));
}

CommandListCommand* CommandObserver::Unwrap(CommandListCommand* cmd)
{
    if (ObservedCommand* o = dynamic_cast<ObservedCommand*>(cmd)) return o->inner.get();
    return cmd;
}

void CommandObserver::ClearBoundLoading()
{
    t_bound_loading = false;
}

bool CommandObserver::BoundLoading() const
{
    return t_bound_loading;
}

void CommandObserver::ClearThisLoading()
{
    t_this_loading = nullptr;
}

CustomResource* CommandObserver::ThisLoading() const
{
    return t_this_loading;
}

CommandObserver::CmdCost* CommandObserver::CostKind(CommandListCommand* c)
{
    const std::string name = CostKindName(c);
    std::lock_guard<std::mutex> l(cost_mutex_);
    for (size_t i = 0; i < cost_names_.size(); ++i)
        if (cost_names_[i] == name) return &costs_[i];
    cost_names_.push_back(name);
    return &costs_.emplace_back();
}

void CommandObserver::CostEnd(CmdCost& c, int64_t t0, uint64_t saved)
{
    const uint64_t el = (uint64_t)(prof_qpc_now() - t0);
    const uint64_t self = el > t_cmd_child ? el - t_cmd_child : 0;
    c.calls.fetch_add(1, std::memory_order_relaxed);
    c.incl.fetch_add(el, std::memory_order_relaxed);
    c.self.fetch_add(self, std::memory_order_relaxed);
    t_cmd_child = saved + el;
}

void CommandObserver::CostMark()
{
    std::lock_guard<std::mutex> l(cost_mutex_);
    cost_base_.assign(costs_.size() * 3, 0);
    for (size_t i = 0; i < costs_.size(); ++i) {
        cost_base_[i * 3 + 0] = costs_[i].calls.load(std::memory_order_relaxed);
        cost_base_[i * 3 + 1] = costs_[i].incl.load(std::memory_order_relaxed);
        cost_base_[i * 3 + 2] = costs_[i].self.load(std::memory_order_relaxed);
    }
}

void CommandObserver::CostReport(FILE* f, int frames)
{
    struct Row {
        std::string name;
        uint64_t calls;
        uint64_t incl;
        uint64_t self;
    };
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> l(cost_mutex_);
        for (size_t i = 0; i < costs_.size(); ++i) {
            Row r{ cost_names_[i], costs_[i].calls.load(std::memory_order_relaxed),
                costs_[i].incl.load(std::memory_order_relaxed), costs_[i].self.load(std::memory_order_relaxed) };
            if (i * 3 + 2 < cost_base_.size()) {
                r.calls -= cost_base_[i * 3 + 0];
                r.incl -= cost_base_[i * 3 + 1];
                r.self -= cost_base_[i * 3 + 2];
            }
            if (r.calls) rows.push_back(std::move(r));
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.self > b.self; });
    const double inv = frames > 0 ? 1.0 / frames : 0.0;
    const double ms = 1000.0 / (double)prof_qpc_freq();
    uint64_t total = 0;
    for (const Row& r : rows) total += r.self;
    std::fprintf(f,
        "3DMIGOTO COMMANDS / frame -- time inside each command by kind, all threads (self leaves out the "
        "commands it runs; %.3f ms/frame in all):\n",
        total * ms * inv);
    std::fprintf(f, "  %-44s %9s %10s %12s %10s\n", "kind", "calls/f", "self ms/f", "self us/call", "incl ms/f");
    for (const Row& r : rows)
        std::fprintf(f, "  %-44s %9.1f %10.3f %12.2f %10.3f\n", r.name.c_str(), r.calls * inv, r.self * ms * inv,
            r.self * ms * 1000.0 / (double)r.calls, r.incl * ms * inv);
}

namespace {
void ObservedCommand::run(CommandListState* state)
{
    const bool timed = g_prof_enabled;
    uint64_t saved = 0;
    int64_t t0 = 0;
    if (timed) {
        saved = t_cmd_child;
        t_cmd_child = 0;
        t0 = prof_qpc_now();
    }
    if (t_observer) t_observer(ini_line.c_str(), t_observer_user);
    if (t_typed && relevant) {
        if (cached_ok) {
            ObservedCmd o = cached;
            o.post = state->post;
            t_typed(o, t_typed_user);
        } else {
            ObservedCmd o;
            o.post = state->post;
            if (ClassifyCommand(inner.get(), state, o)) t_typed(o, t_typed_user);
        }
    }
    const wchar_t* outer = t_cur_ini_line;
    t_cur_ini_line = ini_line.c_str();
    uint32_t before = 0;
    if (sel_var) memcpy(&before, sel_var, sizeof before);
    ID3D11Resource* res_before = sel_res ? sel_res->resource : nullptr;
    const bool null_before = sel_res && sel_res->is_null;
    std::vector<CommandListCommand*>* const trace = is_if ? nullptr : t_trace;
    if (trace) {
        trace->push_back(this);
        t_trace = nullptr;
    }
    if (file_src) {
        PortApi::LoadDeferredNow(file_src);
        NoteTextureUse(file_src->resource);
    }
    bool src_loading = false;
    if (slot_src) {
        if (!slot_src->substantiated)
            PortApi::DeferTexture(slot_src);
        else
            NoteTextureUse(slot_src->resource);
        src_loading = !slot_src->resource && PortApi::DeferredPending(slot_src);
        if (src_loading) t_bound_loading = true;
    }
    if (this_copy) t_this_loading = src_loading ? slot_src : nullptr;
    ResourceCopyOperation* const held = this_copy && state->resource && !state->this_target ? this_copy : nullptr;
    DeferredBinding bound;
    if (held) held->deferred = &bound;
    if (!is_run || !g_command_observer.RunExplicit(inner.get(), state)) inner->run(state);
    if (held) {
        held->deferred = nullptr;
        if (bound.assigned) *state->resource = bound.resource;
        if (bound.resource) bound.resource->Release();
        if (bound.view) bound.view->Release();
    }
    if (trace) t_trace = trace;
    t_cur_ini_line = outer;
    if (sel_always)
        g_selection_watch.BumpInputs();
    else if (sel_res) {
        if ((sel_res->resource != res_before || sel_res->is_null != null_before) &&
            g_selection_watch.ResWatched(sel_res))
            g_selection_watch.BumpInputs();
    } else if (sel_var) {
        uint32_t after;
        memcpy(&after, sel_var, sizeof after);
        if (after != before) {
            const unsigned g = g_selection_watch.VarsGen();
            if (g != sel_watch_gen) {
                sel_watched = g_selection_watch.VarWatched(sel_var);
                sel_watch_gen = g;
            }
            if (sel_watched) g_selection_watch.BumpInputs();
        }
    }
    if (timed) g_command_observer.CostEnd(*cost, t0, saved);
}
} // namespace

void WrapObservedCommands(std::vector<CommandList*>& lists)
{
    g_command_observer.WrapAll(lists);
}

void WrapObservedList(CommandList* list)
{
    g_command_observer.Wrap(list);
}

} // namespace MigotoPort

namespace PortApi {

void ClearBoundLoading()
{
    MigotoPort::g_command_observer.ClearBoundLoading();
}

bool BoundLoading()
{
    return MigotoPort::g_command_observer.BoundLoading();
}

void CommandCostMark()
{
    MigotoPort::g_command_observer.CostMark();
}

void CommandCostReport(FILE* f, int frames)
{
    MigotoPort::g_command_observer.CostReport(f, frames);
}

} // namespace PortApi
