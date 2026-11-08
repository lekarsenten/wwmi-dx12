#pragma once
// HOT PATH -- which recorded command list each submission waits for, and how much of that list's
// recording is the add-on's.
//
// Every registered ReShade handler runs inside a HotPath::Scope (HP() at registration), which times it
// exclusive of nested handlers and charges it to the command list the handler was given. A trace that
// enables the WWMI-DX12-HotPath provider gets ListReset / ListClose / Execute / Frame events to join with
// its CSwitch and ReadyThread (tools/hot_path_etw.py). Debug build only; idle unless a trace enables it.
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <windows.h>
#include "../core/sharded_map.h"

namespace reshade::api {
struct command_list;
struct command_queue;
}
struct ID3D12CommandQueue;
struct ID3D12Fence;
struct ID3D12GraphicsCommandList;

// clang-format off
#define WWMI_HP_HOOKS(X) \
    X(destroy_device) \
    X(destroy_swapchain) \
    X(create_resource) \
    X(init_resource) \
    X(update_texture_region) \
    X(copy_buffer_to_texture) \
    X(copy_texture_region) \
    X(copy_resource) \
    X(copy_buffer_region) \
    X(barrier) \
    X(execute_command_list) \
    X(destroy_resource) \
    X(init_resource_view) \
    X(update_descriptor_tables) \
    X(copy_descriptor_tables) \
    X(create_pipeline) \
    X(init_pipeline) \
    X(init_pipeline_layout) \
    X(destroy_pipeline_layout) \
    X(destroy_pipeline) \
    X(bind_pipeline) \
    X(bind_descriptor_tables) \
    X(reset_command_list) \
    X(close_command_list) \
    X(build_acceleration_structure) \
    X(dispatch_rays) \
    X(push_descriptors) \
    X(push_constants) \
    X(bind_vertex_buffers) \
    X(bind_index_buffer) \
    X(bind_render_targets) \
    X(bind_viewports) \
    X(bind_scissor_rects) \
    X(bind_pipeline_states) \
    X(draw_indexed) \
    X(dispatch) \
    X(present)

enum HpHook {
#define WWMI_HP_ENUM(n) HK_##n,
    WWMI_HP_HOOKS(WWMI_HP_ENUM)
#undef WWMI_HP_ENUM
    HK_COUNT
};
// clang-format on

#ifdef WWMI_DEBUG
namespace HotPath {

inline std::atomic<bool> g_on{ false };

struct Rec;
Rec* RecFor(reshade::api::command_list* cl);
void HookDone(int hook, uint64_t self_ns, Rec* rec);
inline thread_local Rec* t_rec = nullptr;

inline std::atomic<bool> g_gpu_on{ false };
inline std::atomic<bool> g_gpu_detail{ false };
struct GpuRec;
GpuRec*& GpuOf(Rec* r);
void GpuHook(Rec* rec, int hook, reshade::api::command_list* cl);
void GpuListReset(Rec* rec);
void GpuListClose(Rec* rec, reshade::api::command_list* cl, uint32_t claimed);
void GpuExecuted(Rec* rec, reshade::api::command_queue* queue);
void GpuNoteBarriers(uint32_t count);
void* GpuBracketBegin(ID3D12GraphicsCommandList* cl, int bucket);
void GpuBracketEnd(ID3D12GraphicsCommandList* cl, void* token);
void GpuOwnListBegin(ID3D12GraphicsCommandList* cl);
void GpuOwnListEnd(ID3D12GraphicsCommandList* cl, ID3D12CommandQueue* queue);
void GpuFrameSubmitted(ID3D12Fence* fence, uint64_t value, ID3D12CommandQueue* queue);
void GpuTick();

void Init();
void OnReset(reshade::api::command_list* cl);
void OnClose(reshade::api::command_list* cl);
void OnExecute(reshade::api::command_queue* queue, reshade::api::command_list* cl);
void NoteClaimed();
void FrameEnd();

inline reshade::api::command_list* ListArg()
{
    return nullptr;
}
template <typename T, typename... R> inline reshade::api::command_list* ListArg(T t, R...)
{
    if constexpr (std::is_same_v<T, reshade::api::command_list*>)
        return t;
    else
        return nullptr;
}

struct Scope;
inline thread_local Scope* t_cur = nullptr;
struct Scope {
    int k;
    int64_t t0 = 0;
    uint64_t child = 0;
    Scope* prev = nullptr;
    Rec* rec = nullptr;
    Rec* prev_rec = nullptr;
    Scope(int hook, reshade::api::command_list* cl)
        : k(g_on.load(std::memory_order_relaxed) ? hook : -1)
    {
        if (k < 0) return;
        prev = t_cur;
        t_cur = this;
        prev_rec = t_rec;
        rec = cl ? RecFor(cl) : nullptr;
        t_rec = rec;
        if (rec && g_gpu_on.load(std::memory_order_relaxed)) GpuHook(rec, hook, cl);
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        t0 = li.QuadPart;
    }
    ~Scope()
    {
        if (k < 0) return;
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        static const double ns_per_tick = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return 1e9 / (double)f.QuadPart;
        }();
        const uint64_t ns = (uint64_t)((double)(li.QuadPart - t0) * ns_per_tick);
        t_cur = prev;
        t_rec = prev_rec;
        if (prev) prev->child += ns;
        HookDone(k, ns > child ? ns - child : 0, rec);
    }
};

template <auto Fn, int K> struct Wrap;
template <typename R, typename... A, R (*Fn)(A...), int K> struct Wrap<Fn, K> {
    static R call(A... a)
    {
        Scope s(K, ListArg(a...));
        return Fn(a...);
    }
};

} // namespace HotPath
#define HP(name) (&HotPath::Wrap<&on_##name, HK_##name>::call)
#else
namespace HotPath {
inline void Init() {}
inline void OnReset(reshade::api::command_list*) {}
inline void OnClose(reshade::api::command_list*) {}
inline void OnExecute(reshade::api::command_queue*, reshade::api::command_list*) {}
inline void NoteClaimed() {}
inline void FrameEnd() {}
inline void GpuNoteBarriers(uint32_t) {}
inline void* GpuBracketBegin(ID3D12GraphicsCommandList*, int)
{
    return nullptr;
}
inline void GpuBracketEnd(ID3D12GraphicsCommandList*, void*) {}
inline void GpuOwnListBegin(ID3D12GraphicsCommandList*) {}
inline void GpuOwnListEnd(ID3D12GraphicsCommandList*, ID3D12CommandQueue*) {}
inline void GpuFrameSubmitted(ID3D12Fence*, uint64_t, ID3D12CommandQueue*) {}
inline void GpuTick() {}
}
#define HP(name) (&on_##name)
#endif
