#include "shim_unmet.h"
#include "../core/log.h"

namespace MigotoPort {

ShimUnmet& g_shim_unmet = *new ShimUnmet();

void ShimUnmet::Note(const wchar_t* what, const wchar_t* detail)
{
    // The count feeds only the debug heartbeat and the key/mutex/map only the log -- both off with logging off.
    if (!logger::enabled(1)) return;
    frame_count_.fetch_add(1, std::memory_order_relaxed);
    std::wstring key = std::wstring(what ? what : L"?") + L"|" + (detail ? detail : L"");
    bool first;
    {
        std::lock_guard<std::mutex> l(mutex_);
        first = (++seen_[key] == 1);
    }
    if (first)
        logger::write("SHIM-UNMET: %S (%S) -- the mod asked and we bound nothing; the game's own binding stays",
            what ? what : L"?", detail ? detail : L"");
}

unsigned ShimUnmet::TakeCount()
{
    return frame_count_.exchange(0, std::memory_order_relaxed);
}

} // namespace MigotoPort
