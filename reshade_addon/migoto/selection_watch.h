#pragma once
// What a section's selection depends on: the generation a memoised run is keyed by, bumped when a watched
// variable or resource changes.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace MigotoPort {

class SelectionWatch {
public:
    unsigned InputsGen() const;
    void BumpInputs();
    void WatchVar(const float* v);
    bool VarWatched(const float* v);
    unsigned VarsGen() const;
    void SweepVars();
    void WatchRes(const void* cr);
    bool ResWatched(const void* cr);
    void Clear();

private:
    struct SeenVar {
        const float* v;
        uint32_t bits;
    };
    std::atomic<unsigned> inputs_gen_{ 1 };
    std::atomic<unsigned> vars_gen_{ 1 };
    std::mutex mutex_;
    std::unordered_set<const float*> vars_;
    std::vector<SeenVar> seen_;
    std::unordered_set<const void*> res_;
};
extern SelectionWatch g_selection_watch;

} // namespace MigotoPort
