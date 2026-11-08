// port_api: their override sections and command lists, run for a draw the addon hands in.
#include "port_api_internal.h"
#include "globals.h"
#include "IniHandler.h"
#include "CommandList.h"
#include "ResourceHash.h"
#include "DrawCallInfo.h"
#include "HackerDevice.h"
#include "HackerContext.h"
#include "frame_analysis_engine.h"
#include "profiling.h"
#include "command_observer.h"
#include "selection_watch.h"
#include "port_toggles.h" // g_port_post_split
#include "dx12_context.h"
#include "dx12_context_impl.h"
#include "dx12_pipeline.h"
#include "dx12_wrap.h"
#include "../core/log.h"
#include "prof_core.h" // PROF(PL_*): the engine-lock WAITs measured apart from the work, the lookups

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace PortApi {

namespace {

DrawCallInfo ToCallInfo(const DrawInfo* d)
{
    DrawCallInfo ci;
    if (!d) return ci;
    if (d->dispatch) {
        ci.type = DrawCall::Dispatch;
        ci.ThreadGroupCountX = d->thread_group_x;
        ci.ThreadGroupCountY = d->thread_group_y;
        ci.ThreadGroupCountZ = d->thread_group_z;
        return ci;
    }
    ci.type = d->indexed ? DrawCall::DrawIndexed : DrawCall::Draw;
    ci.VertexCount = d->vertex_count;
    ci.IndexCount = d->index_count;
    ci.InstanceCount = d->instance_count;
    ci.FirstVertex = d->first_vertex;
    ci.FirstIndex = d->first_index;
    ci.FirstInstance = d->first_instance;
    return ci;
}

bool ReplayExpr(const CommandListEvaluatable* e, std::vector<const float*>& vars, std::vector<CustomResource*>& ress)
{
    if (const CommandListOperand* o = dynamic_cast<const CommandListOperand*>(e)) {
        if (o->type == ParamOverrideType::VALUE) return true;
        if (o->type == ParamOverrideType::VARIABLE) {
            if (!o->var_ftarget) return false;
            vars.push_back(o->var_ftarget);
            return true;
        }
        if (o->type != ParamOverrideType::TEXTURE) return false;
        const ResourceCopyTarget& t = o->texture_filter_target;
        if (t.type != ResourceCopyTargetType::CUSTOM_RESOURCE || t.custom_resource_pool ||
            t.pool_dynamic_index_expression || t.evaluation_mode != ResourceCopyTargetEvaluationMode::RESOURCE)
            return false;
        CustomResource* cr = const_cast<ResourceCopyTarget&>(t).GetCustomResource(nullptr);
        if (!cr) return false;
        ress.push_back(cr);
        return true;
    }
    if (const CommandListOperator* op = dynamic_cast<const CommandListOperator*>(e))
        return (!op->lhs || ReplayExpr(op->lhs.get(), vars, ress)) &&
            (!op->rhs || ReplayExpr(op->rhs.get(), vars, ress));
    return false;
}

bool ReplayList(CommandList* cl, bool post, std::vector<const float*>& vars, std::vector<CustomResource*>& ress)
{
    if (!cl || cl->ResolveCommandList() != cl) return false;
    for (const auto& cmd : cl->commands) {
        CommandListCommand* c = MigotoPort::g_command_observer.Unwrap(cmd.get());
        if (c == cmd.get()) return false;
        auto* ic = dynamic_cast<IfCommand*>(c);
        if (!ic) continue;
        if (!ic->expression.evaluatable || !ReplayExpr(ic->expression.evaluatable.get(), vars, ress)) return false;
        CommandList* t = post ? ic->true_commands_post.get() : ic->true_commands_pre.get();
        CommandList* f = post ? ic->false_commands_post.get() : ic->false_commands_pre.get();
        if (!ReplayList(t, post, vars, ress) || !ReplayList(f, post, vars, ress)) return false;
    }
    return true;
}

struct ResSnap {
    ID3D11Resource* res;
    bool is_null;
};

struct Trace {
    bool ok = false;
    std::vector<CustomResource*> ress;
    std::vector<ResSnap> snap;
    std::vector<CommandListCommand*> leaves;
    unsigned gen = 0;
};

Trace* TraceOf(CommandList* list, bool post)
{
    if (G->analyse_frame || MigotoPort::g_command_observer.Active() || Profiling::mode != Profiling::Mode::NONE)
        return nullptr;
    if (!list || list->ResolveCommandList() != list) return nullptr;
    static std::unordered_map<uintptr_t, Trace> s_traces;
    static unsigned s_gen = ~0u;
    const unsigned gen = LoadGeneration();
    if (gen != s_gen) {
        s_traces.clear();
        s_gen = gen;
    }
    const uintptr_t key = (uintptr_t)list | (post ? 1 : 0);
    auto it = s_traces.find(key);
    if (it == s_traces.end()) {
        Trace t;
        std::vector<const float*> vars;
        t.ok = ReplayList(list, post, vars, t.ress);
        if (!t.ok)
            t.ress.clear();
        else
            for (const float* v : vars) MigotoPort::g_selection_watch.WatchVar(v);
        it = s_traces.emplace(key, std::move(t)).first;
    }
    return it->second.ok ? &it->second : nullptr;
}

void TakeSnap(const Trace& t, std::vector<ResSnap>& out)
{
    out.resize(t.ress.size());
    for (size_t i = 0; i < t.ress.size(); ++i) out[i] = ResSnap{ t.ress[i]->resource, t.ress[i]->is_null };
}

bool SnapHolds(const Trace& t, const std::vector<ResSnap>& snap)
{
    if (snap.size() != t.ress.size()) return false;
    for (size_t i = 0; i < t.ress.size(); ++i)
        if (t.ress[i]->resource != snap[i].res || t.ress[i]->is_null != snap[i].is_null) return false;
    return true;
}

void ReplayLeaves(Trace& t, DrawCallInfo* ci, bool post)
{
    CommandListState state;
    state.mHackerDevice = &g_dev;
    state.mHackerContext = &g_ctx;
    state.mOrigDevice1 = g_dev.GetPassThroughOrigDevice1();
    state.mOrigContext1 = g_ctx.GetPassThroughOrigContext1();
    state.call_info = ci;
    state.resource = ci ? (ID3D11Resource**)ci->indirect_buffer : nullptr;
    state.view = nullptr;
    state.post = post;

    if (!t.leaves.empty()) {
        state.recursion++;
        for (size_t i = 0; i < t.leaves.size() && !state.aborted; ++i) t.leaves[i]->run(&state);
        state.recursion--;
    }

    if (state.update_params && g_dev.mIniTexture) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = state.mOrigContext1->Map(g_dev.mIniTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            LogInfo("CommandListFlushState: Map failed\n");
            return;
        }
        memcpy(mapped.pData, G->iniParams.data(), sizeof(DirectX::XMFLOAT4) * G->iniParams.size());
        state.mOrigContext1->Unmap(g_dev.mIniTexture, 0);
        state.update_params = false;
        Profiling::iniparams_updates++;
    }
}

enum ReplayCount { RC_TopReplayed, RC_TopRecorded, RC_TopPlain, RC_SubReplayed, RC_SubRecorded, RC_SubPlain, RC_COUNT };
unsigned g_replay_n[RC_COUNT] = {};

void RunList(CommandList* list, DrawCallInfo* ci, bool post)
{
    Trace* t = TraceOf(list, post);
    if (!t) {
        ++g_replay_n[RC_TopPlain];
        RunCommandList(&g_dev, &g_ctx, list, ci, post);
        return;
    }
    const unsigned gen = MigotoPort::g_selection_watch.InputsGen();
    if (t->gen == gen && SnapHolds(*t, t->snap)) {
        ++g_replay_n[RC_TopReplayed];
        ReplayLeaves(*t, ci, post);
        return;
    }
    ++g_replay_n[RC_TopRecorded];
    t->gen = 0;
    t->leaves.clear();
    TakeSnap(*t, t->snap);
    std::vector<CommandListCommand*>* const outer = MigotoPort::g_command_observer.SetTrace(&t->leaves);
    RunCommandList(&g_dev, &g_ctx, list, ci, post);
    MigotoPort::g_command_observer.SetTrace(outer);
    if (MigotoPort::g_selection_watch.InputsGen() != gen || !SnapHolds(*t, t->snap)) return;
    t->gen = gen;
}

bool RunExplicitHook(CommandListCommand* inner, CommandListState* state)
{
    auto* rc = static_cast<RunExplicitCommandList*>(inner);
    if (MigotoPort::g_present_redraw || !rc->command_list_section || rc->run_pre_and_post_together ||
        state->recursion > MAX_COMMAND_LIST_RECURSION)
        return false;
    CommandList* list =
        state->post ? &rc->command_list_section->post_command_list : &rc->command_list_section->command_list;
    Trace* t = TraceOf(list, state->post);
    if (!t) {
        ++g_replay_n[RC_SubPlain];
        return false;
    }
    const unsigned gen = MigotoPort::g_selection_watch.InputsGen();
    if (t->gen == gen && SnapHolds(*t, t->snap)) {
        ++g_replay_n[RC_SubReplayed];
        if (!t->leaves.empty()) {
            state->recursion++;
            for (size_t i = 0; i < t->leaves.size() && !state->aborted; ++i) t->leaves[i]->run(state);
            state->recursion--;
        }
        return true;
    }
    ++g_replay_n[RC_SubRecorded];
    t->gen = 0;
    t->leaves.clear();
    TakeSnap(*t, t->snap);
    std::vector<CommandListCommand*>* const outer = MigotoPort::g_command_observer.SetTrace(&t->leaves);
    inner->run(state);
    MigotoPort::g_command_observer.SetTrace(outer);
    if (MigotoPort::g_selection_watch.InputsGen() == gen && SnapHolds(*t, t->snap)) t->gen = gen;
    return true;
}

const bool s_run_hook_set = (MigotoPort::CommandObserverInstance().SetRunExplicitHook(&RunExplicitHook), true);

} // namespace

// ---- PRE/POST SPLIT -------------------------------------------------------------------------------
//
// Their Before* stashes the matched section's post list and After* runs it -- ACROSS the draw
// (HackerContext.cpp BeforeDispatch/AfterDispatch, BeforeDraw/AfterDraw).
//
// It is not cosmetic. RabbitFX's [CommandListSetTextures] saves the game's texture
// (`ResourceTempT7 = ref ps-t7`), binds the mod's (`ps-t7 = ref ResourceDiffuse`), and ends with
// `post run = CommandListResetTextures`, which puts the GAME's texture back. Running that post list
// before the draw means the mod texture is bound and restored again with no draw in between -- the draw
// samples VANILLA.
//
// A queue rather than a single slot: several sections can match one draw (TextureOverride matches are
// sorted by match_priority and ALL of them run), so every matched post list has to be held and then run
// in the same order theirs would.
namespace {

struct PendingPost {
    CommandList* list;
    DrawCallInfo ci; // by value: theirs passes the same call_info to both phases, and the caller's
    // DrawCallInfo is a stack local that is gone by the time After* runs.
};
thread_local std::vector<PendingPost> t_pending_post;
thread_local std::vector<PendingPost> t_late_post;
thread_local bool t_post_late = false;

void QueuePost(CommandList* list, const DrawCallInfo& ci)
{
    if (!list) return;
    if (!MigotoPort::g_port_post_split) {
        DrawCallInfo tmp = ci;
        RunList(list, &tmp, true);
        return;
    }
    std::vector<PendingPost>& queue = t_post_late ? t_late_post : t_pending_post;
    if (queue.size() == 64) {
        static bool once = false;
        if (!once) {
            once = true;
            logger::write("PENDING-POST LEAK: %zu queued post lists with no drain -- a QueuePost site is "
                          "missing its RunPendingPost()",
                queue.size());
        }
    }
    queue.push_back({ list, ci });
}

} // namespace

LateStagePost::LateStagePost()
    : was_(t_post_late)
{
    t_post_late = true;
}

LateStagePost::~LateStagePost()
{
    t_post_late = was_;
}

void RunPendingPost()
{
    if (t_pending_post.empty() && t_late_post.empty()) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    std::vector<PendingPost> run;
    run.swap(t_pending_post);
    run.insert(run.end(), t_late_post.begin(), t_late_post.end());
    t_late_post.clear();
    for (PendingPost& p : run) RunList(p.list, &p.ci, true);
}

void RunResolvedCommandList(const void* explicit_section, DrawInfo* draw)
{
    std::unique_lock<std::recursive_mutex> engine_lk(EngineLock(), std::defer_lock);
    {
        PROF(PL_SK_Lock);
        engine_lk.lock();
    }
    if (!g_loaded || !explicit_section) return;
    ExplicitCommandListSection* s = (ExplicitCommandListSection*)explicit_section;
    DrawCallInfo ci = ToCallInfo(draw);
    RunList(&s->command_list, &ci, false);
    QueuePost(&s->post_command_list, ci);
    if (draw && ci.skip) draw->skip = true;
}

bool DispatchShaderOverride(uint64_t shader_hash, DrawInfo* draw)
{
    if (!OverrideHashKnown(shader_hash)) return false;
    std::unique_lock<std::recursive_mutex> engine_lk(EngineLock(), std::defer_lock);
    {
        PROF(PL_Disp_Lock);
        engine_lk.lock();
    } // the wait, measured apart from the work (see SK:Lock)
    if (!g_loaded) return false;
    if (!G->fix_enabled) return false;
    ShaderOverrideMap::iterator it = lookup_shaderoverride(shader_hash);
    if (it == G->mShaderOverrideMap.end()) return false;

    DrawCallInfo ci = ToCallInfo(draw);

    bool use_orig = false;
    ShaderOverride& so = it->second;
    if (so.depth_filter != DepthBufferFilter::NONE && draw && draw->depth_known) {
        if (so.depth_filter == DepthBufferFilter::DEPTH_ACTIVE && !draw->depth_active)
            use_orig = true;
        else if (so.depth_filter == DepthBufferFilter::DEPTH_INACTIVE && draw->depth_active)
            use_orig = true;
    }
    if (use_orig) {
        static bool once = false;
        if (!once) {
            once = true;
            logger::write("SHADEROVR: the legacy depth_filter asked for the ORIGINAL shader; replacement is "
                          "the DXIL wall, so only the decision is portable. The command list still runs -- "
                          "theirs runs it before the use_orig branch as well (HackerContext.cpp "
                          "ProcessShaderOverride).");
        }
    }

    RunList(&it->second.command_list, &ci, false);
    QueuePost(&it->second.post_command_list, ci); // runs in RunPendingPost, after the dispatch
    if (draw && ci.skip) draw->skip = true;
    return true;
}

namespace {

void FindTextureOverridesByHash(uint32_t hash, TextureOverrideMatches* matches, DrawCallInfo* call_info)
{
    TextureOverrideMap::iterator i = lookup_textureoverride(hash);
    if (i == G->mTextureOverrideMap.end()) return;
    for (TextureOverride& to : i->second) {
        if (!to.has_draw_context_match) {
            matches->push_back(&to);
            continue;
        }
        if (!call_info) continue; // section wants draw context and we have none: no match, not a crash
        if (!to.match_first_vertex.matches_uint(call_info->FirstVertex)) continue;
        if (!to.match_first_index.matches_uint(call_info->FirstIndex)) continue;
        if (!to.match_first_instance.matches_uint(call_info->FirstInstance)) continue;
        if (!to.match_vertex_count.matches_uint(call_info->VertexCount)) continue;
        if (!to.match_index_count.matches_uint(call_info->IndexCount)) continue;
        if (!to.match_instance_count.matches_uint(call_info->InstanceCount)) continue;
        matches->push_back(&to);
    }
}

} // namespace

bool ShaderOverrideMatches(uint64_t shader_hash)
{
    return OverrideHashKnown(shader_hash);
}

// ---- currently-bound shader state -------------------------------------------------------------
// Their CommandList.cpp process_shader_filter() -- the bare `cs`/`vs`/`ps` operand behind every
// `if cs == 3381.4444` -- resolves in three steps:
//     mHackerContext->mCurrent<Stage>ShaderHandle   ->  lookup_shader_hash()  ->  filter_index
// It only ever READS those; SetBoundShader below is what fills them in. With no handle,
// `if (!shader) return -0.0;` takes, silently, because -0.0 is a legitimate value, not an error.
//
// `ini_hash` is the hash to publish into THEIR map -- i.e. the space their [ShaderOverride] sections
// are keyed in. The caller decides: unmapped DX12 hashes publish as themselves and simply find no
// override (process_shader_filter then returns positive 0.0, "bound but no ShaderOverride", which is
// the honest answer), while a shader we have positively identified can publish the DX11 hash the ini
// keys on. We do not guess that here -- a wrong identification would make a `handling = skip` section
// cancel a real game dispatch.
//
// The handle is opaque: their code uses it only as a map key and as a non-null test (globals.h
// lookup_shader_hash is a plain map lookup), never dereferencing it. We hand out one stable token per hash rather than
// casting the hash to a pointer, so the value is a real address we own.
namespace {
std::unordered_map<uint64_t, uint64_t>& ShaderTokens()
{
    static std::unordered_map<uint64_t, uint64_t> m; // ini_hash -> token storage (address is the handle)
    return m;
}
std::mutex g_shader_state_mutex;
}

void RegisterShaderHash(uint64_t ini_hash)
{
    if (!g_loaded || !ini_hash) return;
    std::lock_guard<std::mutex> lk(g_shader_state_mutex);
    auto it = ShaderTokens().find(ini_hash);
    if (it != ShaderTokens().end()) return; // already registered: no write, no rehash
    uint64_t& slot = ShaderTokens()[ini_hash];
    slot = ini_hash; // content is for debugging; the ADDRESS is the key
    G->mShaders[reinterpret_cast<ID3D11DeviceChild*>(&slot)] = ini_hash;
}

void SetBoundShader(char stage, uint64_t ini_hash)
{
    if (!g_loaded) return;

    ID3D11DeviceChild* handle = nullptr;
    if (ini_hash) {
        std::lock_guard<std::mutex> lk(g_shader_state_mutex);
        auto it = ShaderTokens().find(ini_hash);
        if (it != ShaderTokens().end()) handle = reinterpret_cast<ID3D11DeviceChild*>(&it->second);
    }

    switch (stage) {
    case 'v':
        g_ctx.mCurrentVertexShaderHandle = (ID3D11VertexShader*)handle;
        g_ctx.mCurrentVertexShader = ini_hash;
        break;
    case 'h':
        g_ctx.mCurrentHullShaderHandle = (ID3D11HullShader*)handle;
        g_ctx.mCurrentHullShader = ini_hash;
        break;
    case 'd':
        g_ctx.mCurrentDomainShaderHandle = (ID3D11DomainShader*)handle;
        g_ctx.mCurrentDomainShader = ini_hash;
        break;
    case 'g':
        g_ctx.mCurrentGeometryShaderHandle = (ID3D11GeometryShader*)handle;
        g_ctx.mCurrentGeometryShader = ini_hash;
        break;
    case 'p':
        g_ctx.mCurrentPixelShaderHandle = (ID3D11PixelShader*)handle;
        g_ctx.mCurrentPixelShader = ini_hash;
        break;
    case 'c':
        g_ctx.mCurrentComputeShaderHandle = (ID3D11ComputeShader*)handle;
        g_ctx.mCurrentComputeShader = ini_hash;
        break;
    default: break;
    }
}

float BoundShaderFilter(char stage, uint64_t* out_hash, int* out_have_handle, int* out_in_mshaders)
{
    if (out_hash) *out_hash = 0;
    if (out_have_handle) *out_have_handle = 0;
    if (out_in_mshaders) *out_in_mshaders = 0;
    if (!g_loaded) return -0.0f;

    // Same switch as theirs, same "negative zero means no shader bound" convention.
    ID3D11DeviceChild* handle = nullptr;
    uint64_t h = 0;
    switch (stage) {
    case 'v':
        handle = g_ctx.mCurrentVertexShaderHandle;
        h = g_ctx.mCurrentVertexShader;
        break;
    case 'h':
        handle = g_ctx.mCurrentHullShaderHandle;
        h = g_ctx.mCurrentHullShader;
        break;
    case 'd':
        handle = g_ctx.mCurrentDomainShaderHandle;
        h = g_ctx.mCurrentDomainShader;
        break;
    case 'g':
        handle = g_ctx.mCurrentGeometryShaderHandle;
        h = g_ctx.mCurrentGeometryShader;
        break;
    case 'p':
        handle = g_ctx.mCurrentPixelShaderHandle;
        h = g_ctx.mCurrentPixelShader;
        break;
    case 'c':
        handle = g_ctx.mCurrentComputeShaderHandle;
        h = g_ctx.mCurrentComputeShader;
        break;
    default: return -0.0f;
    }
    if (out_hash) *out_hash = h;
    if (!handle) return -0.0f;
    if (out_have_handle) *out_have_handle = 1;

    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    ShaderMap::iterator si = G->mShaders.find(handle);
    if (si == G->mShaders.end()) return 0.0f;
    if (out_in_mshaders) *out_in_mshaders = 1;

    ShaderOverrideMap::iterator ov = lookup_shaderoverride(si->second);
    if (ov == G->mShaderOverrideMap.end()) return 0.0f;
    if (ov->second.filter_index != FLT_MAX) return ov->second.filter_index;
    return 1.0f; // matched, but the section declares no filter_index
}

void ShaderOverrideSection(uint64_t shader_hash, wchar_t* out, size_t out_chars)
{
    if (!out || !out_chars) return;
    out[0] = 0;
    if (!g_loaded) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    ShaderOverrideMap::iterator it = lookup_shaderoverride(shader_hash);
    if (it == G->mShaderOverrideMap.end()) return;
    wcsncpy_s(out, out_chars, it->second.first_ini_section.c_str(), _TRUNCATE);
}

namespace {
struct SectionHit {
    TextureOverride* to = nullptr;
    ExplicitCommandListSection* ex = nullptr;
};

bool SameName(const std::wstring& a, const wchar_t* b, size_t nb)
{
    if (a.size() != nb) return false;
    for (size_t i = nb; i-- > 0;)
        if (a[i] != b[i]) return false;
    return true;
}

SectionHit FindSection(const wchar_t* fq)
{
    struct Memo {
        unsigned gen = ~0u;
        std::wstring key;
        SectionHit hit;
    };
    thread_local Memo t_memo[16];
    thread_local unsigned t_next = 0;
    const unsigned gen = LoadGeneration();
    const size_t n = wcslen(fq);
    for (const Memo& m : t_memo)
        if (m.gen == gen && SameName(m.key, fq, n)) return m.hit;
    SectionHit h;
    const std::wstring w(fq, n);
    auto& idx = SectionIndex();
    auto it = idx.find(w);
    if (it != idx.end()) {
        h.to = it->second;
    } else {
        auto& xidx = ExplicitIndex();
        auto xit = xidx.find(w);
        if (xit != xidx.end()) h.ex = xit->second;
    }
    Memo& m = t_memo[t_next++ & 15];
    m.gen = gen;
    m.key = w;
    m.hit = h;
    return h;
}
} // namespace

bool RunSectionByName(const wchar_t* fq_section, DrawInfo* draw)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (draw)
        draw->srv_gen_at_entry = MigotoPort::SrvWriteGen(); // under the lock: nothing of another section's can follow
    if (!g_loaded || !fq_section) return false;
    g_ctx.RestoreInputLayout();
    g_ctx.mDrawPipeline = draw ? draw->pso : 0;
    ProfScope lookup_scope(PL_DI_PsLookup); // name -> section, ended before the list runs
    DrawCallInfo ci = ToCallInfo(draw);
    const SectionHit hit = FindSection(fq_section);
    lookup_scope.End();
    if (hit.to) {
        RunList(&hit.to->command_list, &ci, false);
        QueuePost(&hit.to->post_command_list, ci); // runs in RunPendingPost, after the draw
        if (draw && ci.skip) draw->skip = true;
        return true;
    }
    if (hit.ex) {
        RunList(&hit.ex->command_list, &ci, false);
        QueuePost(&hit.ex->post_command_list, ci); // runs in RunPendingPost, after the draw
        if (draw && ci.skip) draw->skip = true;
        return true;
    }
    return false;
}

bool RunSectionThenPost(const wchar_t* fq_section, DrawInfo* draw)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    const bool ran = RunSectionByName(fq_section, draw);
    RunPendingPost();
    return ran;
}

namespace {
thread_local void (*g_draw_cb)(const ModDraw&, void*) = nullptr;
thread_local void* g_draw_cb_user = nullptr;

void DrawTrampoline(unsigned count, unsigned start, int base, unsigned instances, unsigned start_instance, bool indexed,
    void* /*unused*/)
{
    if (!g_draw_cb) return;
    ModDraw d;
    d.index_count = count;
    d.start_index = start;
    d.base_vertex = base;
    d.instance_count = instances;
    d.start_instance = start_instance;
    d.indexed = indexed;
    g_draw_cb(d, g_draw_cb_user);
}
} // namespace

namespace {
thread_local void (*g_cmd_cb)(const wchar_t*, void*) = nullptr;
thread_local void* g_cmd_cb_user = nullptr;

void CommandTrampoline(const wchar_t* ini_line, void*)
{
    if (!g_cmd_cb || !ini_line) return;
    PROF(PL_PS_Trace);
    g_cmd_cb(ini_line, g_cmd_cb_user);
}
} // namespace

void SetCommandObserver(void (*fn)(const wchar_t* ini_line, void* user), void* user)
{
    g_cmd_cb = fn;
    g_cmd_cb_user = user;
    MigotoPort::g_command_observer.SetObserver(fn ? &CommandTrampoline : nullptr, nullptr);
}

namespace {
thread_local void (*g_typed_cb)(const ObservedCommand&, void*) = nullptr;
thread_local void* g_typed_cb_user = nullptr;

void TypedTrampoline(const MigotoPort::ObservedCmd& in, void*)
{
    if (!g_typed_cb) return;
    PROF(PL_PS_Observe);
    auto key_of = [](std::unordered_map<const void*, std::wstring>& idx, const void* p) -> const wchar_t* {
        if (!p) return nullptr;
        auto it = idx.find(p);
        return (it == idx.end() || it->second.empty()) ? nullptr : it->second.c_str();
    };
    ObservedCommand o;
    if (in.kind == MigotoPort::ObservedCmd::Kind::RunList)
        o.kind = ObservedCommand::Kind::RunList;
    else if (in.kind == MigotoPort::ObservedCmd::Kind::ResourceCopy)
        o.kind = ObservedCommand::Kind::ResourceCopy;
    else
        return;

    o.run_explicit = in.run_explicit; // their resolved section, for RunResolvedCommandList
    // THEIR enum, evaluated HERE, so the addon never carries a copy of the constants that could drift.
    o.dst_is_custom = ((ResourceCopyTargetType)in.dst_type == ResourceCopyTargetType::CUSTOM_RESOURCE);
    o.dst_is_srv = ((ResourceCopyTargetType)in.dst_type == ResourceCopyTargetType::SHADER_RESOURCE);
    o.dst_is_vb = ((ResourceCopyTargetType)in.dst_type == ResourceCopyTargetType::VERTEX_BUFFER);
    o.dst_is_ib = ((ResourceCopyTargetType)in.dst_type == ResourceCopyTargetType::INDEX_BUFFER);
    o.dst_is_this = ((ResourceCopyTargetType)in.dst_type == ResourceCopyTargetType::THIS_RESOURCE);
    o.src_is_custom = ((ResourceCopyTargetType)in.src_type == ResourceCopyTargetType::CUSTOM_RESOURCE);
    o.dst_shader = (char)in.dst_shader;
    o.dst_slot = in.dst_slot;
    o.post = in.post;
    if (o.kind == ObservedCommand::Kind::RunList) {
        o.run_target = key_of(ExplicitNameIndex(), in.run_explicit);
        if (!o.run_target && in.run_target && *in.run_target) o.run_target = in.run_target;
    } else {
        if (o.dst_is_custom) o.dst_resource = key_of(CustomResourceNameIndex(), in.dst_custom);
        if (o.dst_is_custom || o.dst_is_vb || o.dst_is_ib)
            o.src_resource = key_of(CustomResourceNameIndex(), in.src_custom);
        o.src_custom = in.src_custom;
    }
    g_typed_cb(o, g_typed_cb_user);
}
} // namespace

void SetTypedObserver(void (*fn)(const ObservedCommand&, void* user), void* user)
{
    g_typed_cb = fn;
    g_typed_cb_user = user;
    MigotoPort::g_command_observer.SetTypedObserver(fn ? &TypedTrampoline : nullptr, nullptr);
}

void SetDrawCallback(void (*fn)(const ModDraw&, void* user), void* user)
{
    g_draw_cb = fn;
    g_draw_cb_user = user;
    MigotoPort::SetModDrawCallback(fn ? &DrawTrampoline : nullptr, nullptr);
}

int DispatchTextureOverride(uint32_t texture_hash, DrawInfo* draw)
{
    if (!TextureOverrideHashKnown(texture_hash)) return 0; // no section for this hash: nothing to run, no lock
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return 0;

    DrawCallInfo ci = ToCallInfo(draw);

    TextureOverrideMatches matches;
    FindTextureOverridesByHash(texture_hash, &matches, &ci);

    for (TextureOverride* to : matches) {
        RunList(&to->command_list, &ci, false);
        QueuePost(&to->post_command_list, ci); // runs in RunPendingPost, after the draw
    }
    if (draw && ci.skip) draw->skip = true;
    return (int)matches.size();
}

namespace {

bool ListAssignsThis(const CommandList* cl, std::vector<const void*>& seen)
{
    if (!cl) return false;
    for (const auto& cmd : cl->commands) {
        CommandListCommand* c = MigotoPort::g_command_observer.Unwrap(cmd.get());
        if (auto* rc = dynamic_cast<ResourceCopyOperation*>(c))
            if (rc->dst.type == ResourceCopyTargetType::THIS_RESOURCE) return true;
        if (auto* ic = dynamic_cast<IfCommand*>(c)) {
            if (ListAssignsThis(ic->true_commands_pre.get(), seen) ||
                ListAssignsThis(ic->true_commands_post.get(), seen) ||
                ListAssignsThis(ic->false_commands_pre.get(), seen) ||
                ListAssignsThis(ic->false_commands_post.get(), seen))
                return true;
        }
        if (auto* rc = dynamic_cast<RunExplicitCommandList*>(c)) {
            ExplicitCommandListSection* s = rc->command_list_section;
            if (!s) continue;
            if (std::find(seen.begin(), seen.end(), (const void*)s) != seen.end()) continue;
            seen.push_back(s);
            if (ListAssignsThis(&s->command_list, seen) || ListAssignsThis(&s->post_command_list, seen)) return true;
        }
    }
    return false;
}

void LogSelClass(TextureOverride* to, bool sel)
{
    static std::vector<const TextureOverride*> s_logged;
    static bool s_was_armed = false;
    if (!G->analyse_frame) {
        s_was_armed = false;
        return;
    }
    if (!s_was_armed) {
        s_logged.clear();
        s_was_armed = true;
    }
    if (std::find(s_logged.begin(), s_logged.end(), to) != s_logged.end()) return;
    s_logged.push_back(to);
    {
        std::string cmds;
        for (const auto& cmd : to->command_list.commands) {
            CommandListCommand* c = MigotoPort::g_command_observer.Unwrap(cmd.get());
            cmds += " ";
            cmds += typeid(*c).name();
            if (auto* ic = dynamic_cast<IfCommand*>(c))
                cmds += "{" + std::to_string(ic->true_commands_pre->commands.size()) + "/" +
                    std::to_string(ic->false_commands_pre->commands.size()) + "}";
            if (auto* rc = dynamic_cast<ResourceCopyOperation*>(c))
                cmds += "(dst=" + std::to_string((unsigned)rc->dst.type) + ")";
        }
        logger::write("SELCLASS %ls: assigns this=%d | pre:%s | post=%zu", to->ini_section.c_str(), (int)sel,
            cmds.c_str(), to->post_command_list.commands.size());
    }
}

bool SelectsResource(TextureOverride* to)
{
    static std::unordered_map<const TextureOverride*, bool> s_cache;
    static unsigned s_gen = ~0u;
    const unsigned gen = LoadGeneration();
    if (gen != s_gen) {
        s_cache.clear();
        s_gen = gen;
    }
    auto it = s_cache.find(to);
    bool sel;
    if (it != s_cache.end())
        sel = it->second;
    else {
        std::vector<const void*> seen;
        sel = ListAssignsThis(&to->command_list, seen) || ListAssignsThis(&to->post_command_list, seen);
        s_cache.emplace(to, sel);
    }
    LogSelClass(to, sel);
    return sel;
}

bool ModSwitchedOff(const TextureOverride* to)
{
    static std::unordered_map<const TextureOverride*, const float*> s_switch;
    static unsigned s_gen = ~0u;
    const unsigned gen = LoadGeneration();
    if (gen != s_gen) {
        s_switch.clear();
        s_gen = gen;
    }
    auto it = s_switch.find(to);
    if (it == s_switch.end()) {
        const float* v = nullptr;
        const std::wstring& sec = to->ini_section; // TextureOverride\<namespace>\<name>
        const size_t a = sec.find(L'\\');
        const size_t b = sec.rfind(L'\\');
        if (a != std::wstring::npos && b != std::wstring::npos && b > a) {
            std::wstring key = L"$\\" + sec.substr(a + 1, b - a - 1) + L"\\mod_enabled";
            for (auto& ch : key) ch = (wchar_t)towlower(ch);
            auto vi = command_list_globals.find(key);
            if (vi != command_list_globals.end()) v = &vi->second.fval;
        }
        if (v) MigotoPort::g_selection_watch.WatchVar(v);
        it = s_switch.emplace(to, v).first;
    }
    return it->second && *it->second == 0.f;
}

bool PureExpr(const CommandListEvaluatable* e, std::vector<const float*>& vars)
{
    if (const CommandListOperand* o = dynamic_cast<const CommandListOperand*>(e)) {
        if (o->type == ParamOverrideType::VALUE) return true;
        if (o->type != ParamOverrideType::VARIABLE || !o->var_ftarget) return false;
        vars.push_back(o->var_ftarget);
        return true;
    }
    if (const CommandListOperator* op = dynamic_cast<const CommandListOperator*>(e))
        return (!op->lhs || PureExpr(op->lhs.get(), vars)) && (!op->rhs || PureExpr(op->rhs.get(), vars));
    return false;
}

bool PureSelectList(CommandList* cl, std::vector<CustomResource*>& srcs, std::vector<const float*>& vars, int runs)
{
    if (!cl) return true;
    if (cl->ResolveCommandList() != cl) return false;
    for (const auto& cmd : cl->commands) {
        CommandListCommand* c = MigotoPort::g_command_observer.Unwrap(cmd.get());
        if (auto* ic = dynamic_cast<IfCommand*>(c)) {
            if (!ic->expression.evaluatable || !PureExpr(ic->expression.evaluatable.get(), vars)) return false;
            if (!PureSelectList(ic->true_commands_pre.get(), srcs, vars, runs) ||
                !PureSelectList(ic->true_commands_post.get(), srcs, vars, runs) ||
                !PureSelectList(ic->false_commands_pre.get(), srcs, vars, runs) ||
                !PureSelectList(ic->false_commands_post.get(), srcs, vars, runs))
                return false;
            continue;
        }
        if (auto* re = dynamic_cast<RunExplicitCommandList*>(c)) {
            if (runs >= 8 || !re->command_list_section) return false;
            if (!PureSelectList(&re->command_list_section->command_list, srcs, vars, runs + 1) ||
                !PureSelectList(&re->command_list_section->post_command_list, srcs, vars, runs + 1))
                return false;
            continue;
        }
        auto* rc = dynamic_cast<ResourceCopyOperation*>(c);
        if (!rc || rc->dst.type != ResourceCopyTargetType::THIS_RESOURCE) return false;
        if (rc->src.type == ResourceCopyTargetType::EMPTY) continue;
        const int opts = (int)rc->options;
        const int ref_ok = (int)ResourceCopyOptions::REFERENCE | (int)ResourceCopyOptions::UNLESS_NULL;
        if (rc->src.type != ResourceCopyTargetType::CUSTOM_RESOURCE || rc->src.custom_resource_pool ||
            rc->src.pool_dynamic_index_expression ||
            rc->src.evaluation_mode != ResourceCopyTargetEvaluationMode::RESOURCE ||
            !(opts & (int)ResourceCopyOptions::REFERENCE) || (opts & ~ref_ok))
            return false;
        CustomResource* cr = rc->src.GetCustomResource(nullptr);
        if (!cr) return false;
        srcs.push_back(cr);
    }
    return true;
}

void SelectionWrites(CommandList* cl, std::vector<CommandListVariable*>& vars, std::vector<int>& params, int depth)
{
    if (!cl || depth > 8) return;
    for (const auto& cmd : cl->ResolveCommandList()->commands) {
        CommandListCommand* c = MigotoPort::g_command_observer.Unwrap(cmd.get());
        if (auto* va = dynamic_cast<VariableAssignment*>(c)) {
            if (va->var) vars.push_back(va->var);
        } else if (auto* po = dynamic_cast<ParamOverride*>(c)) {
            if (po->param_idx >= 0) params.push_back(po->param_idx);
        } else if (auto* ic = dynamic_cast<IfCommand*>(c)) {
            SelectionWrites(ic->true_commands_pre.get(), vars, params, depth + 1);
            SelectionWrites(ic->true_commands_post.get(), vars, params, depth + 1);
            SelectionWrites(ic->false_commands_pre.get(), vars, params, depth + 1);
            SelectionWrites(ic->false_commands_post.get(), vars, params, depth + 1);
        } else if (auto* re = dynamic_cast<RunExplicitCommandList*>(c)) {
            if (!re->command_list_section) continue;
            SelectionWrites(&re->command_list_section->command_list, vars, params, depth + 1);
            SelectionWrites(&re->command_list_section->post_command_list, vars, params, depth + 1);
        }
    }
}

struct SelSnap {
    ID3D11Resource* res;
    bool is_null;
};
struct SelEntry {
    bool pure = false;
    std::vector<CustomResource*> srcs;
    unsigned gen = 0;
    ID3D11Resource* pre = nullptr;
    ID3D11Resource* post = nullptr;
    std::vector<SelSnap> snap;
    std::vector<CommandListVariable*> writes; // an impure list's `$var =` targets: put back after each selection,
    std::vector<int> param_writes; // and its `xN =` ini params -- DX11 runs these lists only in draws
    CustomResource* pre_loading = nullptr; // what `this` got while it still loads, when nothing loaded did
    CustomResource* post_loading = nullptr;
};

SelEntry& SelEntryOf(TextureOverride* to)
{
    static std::unordered_map<const TextureOverride*, SelEntry> s_sel;
    static unsigned s_gen = ~0u;
    const unsigned gen = LoadGeneration();
    if (gen != s_gen) {
        s_sel.clear();
        s_gen = gen;
    }
    auto it = s_sel.find(to);
    if (it != s_sel.end()) return it->second;
    SelEntry e;
    std::vector<const float*> vars;
    e.pure =
        PureSelectList(&to->command_list, e.srcs, vars, 0) && PureSelectList(&to->post_command_list, e.srcs, vars, 0);
    if (!e.pure) {
        e.srcs.clear();
        SelectionWrites(&to->command_list, e.writes, e.param_writes, 0);
        SelectionWrites(&to->post_command_list, e.writes, e.param_writes, 0);
        if (logger::enabled(1) && (!e.writes.empty() || !e.param_writes.empty()))
            logger::write(
                "SELWRITES %ls: choosing its texture writes %zu variable(s) and %zu ini param(s) -- put back after "
                "each choice, as DX11 runs this list only in the draws that bind the texture",
                to->ini_section.c_str(), e.writes.size(), e.param_writes.size());
    } else {
        for (const float* v : vars) MigotoPort::g_selection_watch.WatchVar(v);
        for (CustomResource* cr : e.srcs) MigotoPort::g_selection_watch.WatchRes(cr);
    }
    return s_sel.emplace(to, std::move(e)).first->second;
}

bool SnapHolds(const SelEntry& e)
{
    if (e.snap.size() != e.srcs.size()) return false;
    for (size_t i = 0; i < e.srcs.size(); ++i)
        if (e.srcs[i]->resource != e.snap[i].res || e.srcs[i]->is_null != e.snap[i].is_null) return false;
    return true;
}

struct SelLoading {
    CustomResource* res = nullptr;
    unsigned gen = 0;
};
thread_local SelLoading t_sel_loading; // PortApi::SelectionLoading

void TakeList(ID3D11Resource* slot, CustomResource* slot_loading, ID3D11Resource*& picked, CustomResource*& loading)
{
    if (slot) {
        picked = slot;
        loading = nullptr;
    } else if (slot_loading) {
        loading = slot_loading;
    }
}

bool RunSelection(TextureOverride* to, ID3D11Resource*& picked, CustomResource*& loading)
{
    SelEntry& e = SelEntryOf(to);
    if (G->analyse_frame) MigotoPort::g_selection_watch.BumpInputs();
    const unsigned gen = MigotoPort::g_selection_watch.InputsGen();
    const bool memo = e.pure && !G->analyse_frame && !MigotoPort::g_command_observer.Any() &&
        to->command_list.ResolveCommandList() == &to->command_list &&
        to->post_command_list.ResolveCommandList() == &to->post_command_list;
    if (memo && e.gen == gen && SnapHolds(e)) {
        TakeList(e.pre, e.pre_loading, picked, loading);
        TakeList(e.post, e.post_loading, picked, loading);
        return true;
    }
    const bool keep = !e.writes.empty() || !e.param_writes.empty();
    std::vector<float> vars_were;
    std::vector<DirectX::XMFLOAT4> params_were;
    if (keep) {
        vars_were.reserve(e.writes.size());
        for (const CommandListVariable* v : e.writes) vars_were.push_back(v->fval);
        params_were.reserve(e.param_writes.size());
        for (const int p : e.param_writes)
            params_were.push_back(p < (int)G->iniParams.size() ? G->iniParams[p] : DirectX::XMFLOAT4{});
    }
    ID3D11Resource* slot = nullptr;
    MigotoPort::g_command_observer.ClearThisLoading();
    RunResourceCommandList(&g_dev, &g_ctx, &to->command_list, &slot, false);
    e.pre = slot;
    e.pre_loading = slot ? nullptr : MigotoPort::g_command_observer.ThisLoading();
    TakeList(e.pre, e.pre_loading, picked, loading);
    slot = nullptr;
    MigotoPort::g_command_observer.ClearThisLoading();
    RunResourceCommandList(&g_dev, &g_ctx, &to->post_command_list, &slot, true);
    e.post = slot;
    e.post_loading = slot ? nullptr : MigotoPort::g_command_observer.ThisLoading();
    TakeList(e.post, e.post_loading, picked, loading);
    e.gen = 0;
    if (keep) {
        for (size_t i = 0; i < e.writes.size(); ++i) e.writes[i]->fval = vars_were[i];
        for (size_t i = 0; i < e.param_writes.size(); ++i)
            if (e.param_writes[i] < (int)G->iniParams.size()) G->iniParams[e.param_writes[i]] = params_were[i];
    }
    if (!memo) return false;
    e.snap.resize(e.srcs.size());
    for (size_t i = 0; i < e.srcs.size(); ++i) e.snap[i] = SelSnap{ e.srcs[i]->resource, e.srcs[i]->is_null };
    e.gen = gen;
    return true;
}

bool EqNoCase(const wchar_t* a, const wchar_t* b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        wchar_t x = a[i];
        wchar_t y = b[i];
        if (x == y) continue;
        if (x >= 0x80 || y >= 0x80) {
            if (_wcsnicmp(a + i, b + i, 1) != 0) return false;
            continue;
        }
        if (x >= L'A' && x <= L'Z') x += L'a' - L'A';
        if (y >= L'A' && y <= L'Z') y += L'a' - L'A';
        if (x != y) return false;
    }
    return true;
}

} // namespace

bool TextureOverrideAssignsThis(TextureOverride* to)
{
    std::vector<const void*> seen;
    return to && (ListAssignsThis(&to->command_list, seen) || ListAssignsThis(&to->post_command_list, seen));
}

namespace {

void ExprVars(const CommandListEvaluatable* e, std::vector<const float*>& vars)
{
    if (const auto* o = dynamic_cast<const CommandListOperand*>(e)) {
        if (o->type == ParamOverrideType::VARIABLE && o->var_ftarget) vars.push_back(o->var_ftarget);
        return;
    }
    if (const auto* op = dynamic_cast<const CommandListOperator*>(e)) {
        if (op->lhs) ExprVars(op->lhs.get(), vars);
        if (op->rhs) ExprVars(op->rhs.get(), vars);
    }
}

void ListReadVars(const CommandList* cl, std::vector<const float*>& vars, std::vector<const void*>& seen)
{
    if (!cl) return;
    for (const auto& cmd : cl->commands) {
        CommandListCommand* c = MigotoPort::g_command_observer.Unwrap(cmd.get());
        if (auto* ic = dynamic_cast<IfCommand*>(c)) {
            if (ic->expression.evaluatable) ExprVars(ic->expression.evaluatable.get(), vars);
            ListReadVars(ic->true_commands_pre.get(), vars, seen);
            ListReadVars(ic->true_commands_post.get(), vars, seen);
            ListReadVars(ic->false_commands_pre.get(), vars, seen);
            ListReadVars(ic->false_commands_post.get(), vars, seen);
        } else if (auto* rc = dynamic_cast<ResourceCopyOperation*>(c)) {
            const CommandListExpression* idx = rc->src.pool_dynamic_index_expression.get();
            if (idx && idx->evaluatable) ExprVars(idx->evaluatable.get(), vars);
        } else if (auto* re = dynamic_cast<RunExplicitCommandList*>(c)) {
            ExplicitCommandListSection* s = re->command_list_section;
            if (!s || std::find(seen.begin(), seen.end(), (const void*)s) != seen.end()) continue;
            seen.push_back(s);
            ListReadVars(&s->command_list, vars, seen);
            ListReadVars(&s->post_command_list, vars, seen);
        }
    }
}

} // namespace

void TextureOverrideReadVars(TextureOverride* to, std::vector<const float*>& vars)
{
    if (!to) return;
    std::vector<const void*> seen;
    ListReadVars(&to->command_list, vars, seen);
    ListReadVars(&to->post_command_list, vars, seen);
}

void ReplayCounts(unsigned* top_replayed, unsigned* top_recorded, unsigned* top_plain, unsigned* sub_replayed,
    unsigned* sub_recorded, unsigned* sub_plain)
{
    *top_replayed = g_replay_n[RC_TopReplayed];
    *top_recorded = g_replay_n[RC_TopRecorded];
    *top_plain = g_replay_n[RC_TopPlain];
    *sub_replayed = g_replay_n[RC_SubReplayed];
    *sub_recorded = g_replay_n[RC_SubRecorded];
    *sub_plain = g_replay_n[RC_SubPlain];
}

ID3D12Resource* SelectTextureOverride(uint32_t texture_hash, bool* cacheable)
{
    if (cacheable) *cacheable = true;
    t_sel_loading = {};
    if (!TextureOverrideHashKnown(texture_hash)) return nullptr; // no section: cannot select, no lock
    PROF(PL_TexSel_Engine);
    std::unique_lock<std::recursive_mutex> engine_lk(EngineLock(), std::defer_lock);
    {
        PROF(PL_TexSel_Lock);
        engine_lk.lock();
    }
    if (!g_loaded) return nullptr;
    TextureOverrideMatches matches;
    FindTextureOverridesByHash(texture_hash, &matches, nullptr); // exact hash only
    if (matches.empty()) return nullptr;

    ID3D11Resource* picked = nullptr;
    CustomResource* loading = nullptr;
    for (TextureOverride* to : matches) {
        if (!SelectsResource(to) || ModSwitchedOff(to)) continue;
        if (!RunSelection(to, picked, loading) && cacheable) *cacheable = false;
    }
    if (!picked) {
        if (loading) t_sel_loading = SelLoading{ loading, LoadGeneration() };
        return nullptr;
    }

    MigotoPort::WrapResource* wr = MigotoPort::AsWrapResource(picked);
    return wr ? wr->dx12 : nullptr;
}

static bool SectionInNs(const std::wstring& sec, const wchar_t* ns, size_t nlen)
{
    static const wchar_t kPrefix[] = L"TextureOverride\\";
    constexpr size_t plen = _countof(kPrefix) - 1;
    return sec.size() > plen + nlen && EqNoCase(sec.c_str(), kPrefix, plen) && EqNoCase(sec.c_str() + plen, ns, nlen) &&
        sec[plen + nlen] == L'\\';
}

bool TextureOverrideInNs(uint32_t texture_hash, const wchar_t* ns)
{
    if (!ns || !*ns || !TextureOverrideHashKnown(texture_hash)) return false;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return false;
    TextureOverrideMatches matches;
    FindTextureOverridesByHash(texture_hash, &matches, nullptr);
    const size_t nlen = wcslen(ns);
    for (TextureOverride* to : matches)
        if (SectionInNs(to->ini_section, ns, nlen)) return true;
    return false;
}

ID3D12Resource* SelectTextureOverrideNs(uint32_t texture_hash, const wchar_t* ns, bool* cacheable)
{
    if (cacheable) *cacheable = true;
    t_sel_loading = {};
    if (!ns || !*ns) return SelectTextureOverride(texture_hash, cacheable);
    if (!TextureOverrideHashKnown(texture_hash)) return nullptr;
    PROF(PL_TexSel_Engine);
    std::unique_lock<std::recursive_mutex> engine_lk(EngineLock(), std::defer_lock);
    {
        PROF(PL_TexSel_Lock);
        engine_lk.lock();
    }
    if (!g_loaded) return nullptr;
    TextureOverrideMatches matches;
    FindTextureOverridesByHash(texture_hash, &matches, nullptr);
    if (matches.empty()) return nullptr;

    bool own = false;
    ID3D11Resource* picked = nullptr;
    CustomResource* loading = nullptr;
    const size_t nlen = wcslen(ns);
    for (TextureOverride* to : matches) {
        if (!SectionInNs(to->ini_section, ns, nlen)) continue;
        own = true;
        if (!SelectsResource(to) || ModSwitchedOff(to)) continue;
        if (!RunSelection(to, picked, loading) && cacheable) *cacheable = false;
    }
    if (!own) return SelectTextureOverride(texture_hash, cacheable);
    if (!picked) {
        if (loading) t_sel_loading = SelLoading{ loading, LoadGeneration() };
        return nullptr;
    }
    MigotoPort::WrapResource* wr = MigotoPort::AsWrapResource(picked);
    return wr ? wr->dx12 : nullptr;
}

const void* SelectionLoading(unsigned* gen)
{
    if (gen) *gen = t_sel_loading.gen;
    return t_sel_loading.res;
}

uint32_t SwitchOfResource(const wchar_t* res_fq)
{
    if (!res_fq || !*res_fq) return 0;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return 0;
    static std::unordered_map<const CustomResource*, uint32_t> s_switch;
    static unsigned s_gen = ~0u;
    const unsigned gen = LoadGeneration();
    if (gen != s_gen) {
        s_switch.clear();
        s_gen = gen;
        const auto t0 = std::chrono::steady_clock::now();
        size_t sections = 0;
        for (auto& [hash, tos] : G->mTextureOverrideMap)
            for (TextureOverride& to : tos) {
                const SelEntry& e = SelEntryOf(&to);
                std::vector<const CustomResource*> distinct(e.srcs.begin(), e.srcs.end());
                std::sort(distinct.begin(), distinct.end());
                distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
                if (distinct.size() < 2) continue;
                ++sections;
                for (const CustomResource* cr : distinct) s_switch.emplace(cr, hash);
            }
        logger::write(
            "RFX-SWITCHES gen=%u: %zu [TextureOverride] section(s) switch among %zu resource(s), found in %.2f ms", gen,
            sections, s_switch.size(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    auto it = customResources.find(res_fq);
    if (it == customResources.end()) return 0;
    auto sw = s_switch.find(&it->second);
    return sw == s_switch.end() ? 0 : sw->second;
}

void TextureOverrideSection(uint32_t texture_hash, wchar_t* out, size_t out_chars)
{
    if (!out || !out_chars) return;
    out[0] = 0;
    if (!g_loaded || !TextureOverrideHashKnown(texture_hash)) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    TextureOverrideMatches matches;
    FindTextureOverridesByHash(texture_hash, &matches, nullptr); // exact hash only
    if (matches.empty()) return;
    wcsncpy_s(out, out_chars, matches.front()->ini_section.c_str(), _TRUNCATE);
}

} // namespace PortApi
