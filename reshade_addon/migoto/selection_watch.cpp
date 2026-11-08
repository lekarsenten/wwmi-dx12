#include "selection_watch.h"
#include <cstring>

namespace MigotoPort {

SelectionWatch g_selection_watch;

unsigned SelectionWatch::InputsGen() const
{
    return inputs_gen_.load(std::memory_order_acquire);
}

void SelectionWatch::BumpInputs()
{
    inputs_gen_.fetch_add(1, std::memory_order_acq_rel);
}

void SelectionWatch::WatchVar(const float* v)
{
    std::lock_guard<std::mutex> l(mutex_);
    if (!vars_.insert(v).second) return;
    SeenVar s{ v, 0 };
    memcpy(&s.bits, v, sizeof s.bits);
    seen_.push_back(s);
    vars_gen_.fetch_add(1, std::memory_order_acq_rel);
}

bool SelectionWatch::VarWatched(const float* v)
{
    std::lock_guard<std::mutex> l(mutex_);
    return vars_.count(v) != 0;
}

unsigned SelectionWatch::VarsGen() const
{
    return vars_gen_.load(std::memory_order_acquire);
}

void SelectionWatch::SweepVars()
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> l(mutex_);
        for (SeenVar& s : seen_) {
            uint32_t now;
            memcpy(&now, s.v, sizeof now);
            if (now == s.bits) continue;
            s.bits = now;
            changed = true;
        }
    }
    if (changed) BumpInputs();
}

void SelectionWatch::WatchRes(const void* cr)
{
    std::lock_guard<std::mutex> l(mutex_);
    if (res_.insert(cr).second) vars_gen_.fetch_add(1, std::memory_order_acq_rel);
}

bool SelectionWatch::ResWatched(const void* cr)
{
    std::lock_guard<std::mutex> l(mutex_);
    return res_.count(cr) != 0;
}

void SelectionWatch::Clear()
{
    {
        std::lock_guard<std::mutex> l(mutex_);
        vars_.clear();
        seen_.clear();
        res_.clear();
    }
    vars_gen_.fetch_add(1, std::memory_order_acq_rel);
    BumpInputs();
}

} // namespace MigotoPort
