// Submission order: which command list the GPU sees when. See sk_internal.h.
#include "sk_internal.h"
#include "../core/frame_clock.h"

namespace {
struct SubmitLog {
    uint32_t frame = 0;
    std::vector<const void*> order;
};
std::mutex g_sub_mtx;
SubmitLog g_sub_cur;
SubmitLog g_sub_prev;
}
std::atomic<unsigned long long> g_sub_seen{ 0 };

int SubmitIndexOf(uint32_t frame, const void* list)
{
    std::lock_guard<std::mutex> l(g_sub_mtx);
    const SubmitLog* src = nullptr;
    if (g_sub_cur.frame == frame)
        src = &g_sub_cur;
    else if (g_sub_prev.frame == frame)
        src = &g_sub_prev;
    if (!src) return -1;
    for (size_t i = 0; i < src->order.size(); ++i)
        if (src->order[i] == list) return (int)i;
    return -1;
}

void SkNoteSubmitted(const void* list)
{
    g_sub_seen.fetch_add(1); // read only by the SHAPEKEY-ORDER log line
    std::lock_guard<std::mutex> l(g_sub_mtx);
    if (g_sub_cur.frame != g_frame_id) {
        g_sub_prev = std::move(g_sub_cur);
        g_sub_cur.frame = g_frame_id;
        g_sub_cur.order.clear();
    }
    if (g_sub_cur.order.size() < 512) g_sub_cur.order.push_back(list);
}
