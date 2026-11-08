#pragma once
// A mod asked for a binding the shim could not meet, so the game's own stays: logged once per kind, counted per frame.
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace MigotoPort {

class ShimUnmet {
public:
    void Note(const wchar_t* what, const wchar_t* detail);
    // Per-frame total, for the heartbeat. Reading it clears it.
    unsigned TakeCount();

private:
    std::mutex mutex_;
    std::unordered_map<std::wstring, unsigned> seen_;
    std::atomic<unsigned> frame_count_{ 0 };
};
extern ShimUnmet& g_shim_unmet;

} // namespace MigotoPort
