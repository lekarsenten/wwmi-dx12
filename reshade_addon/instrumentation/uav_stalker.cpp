#include "uav_stalker.h"

UavStalker g_uav_stalker;

void UavStalker::NoteCharVb(uint64_t va)
{
    std::lock_guard lock(char_vb_mutex_);
    if (va != 0) char_vb_vas_.insert(va);
}

void UavStalker::ForgetCharVbs()
{
    std::lock_guard lock(char_vb_mutex_);
    char_vb_vas_.clear();
}

void UavStalker::Probe(uint64_t cs_hash, uint32_t root, uint32_t slot, uint64_t res_va)
{
    std::lock_guard lock_vb(char_vb_mutex_);
    for (uint64_t char_va : char_vb_vas_) {
        if (res_va >= char_va - 16777216 && res_va < char_va + 16777216) {
            std::lock_guard lock_res(results_mutex_);
            results_.insert({ cs_hash, root, slot });
        }
    }
}

void UavStalker::Clear()
{
    std::lock_guard lock(results_mutex_);
    results_.clear();
}
