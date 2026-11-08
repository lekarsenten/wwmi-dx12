#pragma once
// debug_pipe.h -- debug builds: commands over \\.\pipe\wwmi_dx12_debug, one per connection, run on the
// present thread. It listens only while the overlay's K7 is on, and K7 starts off at every launch.
// Client: tools/debug_pipe/pipe.py <command>. `help` lists the commands.
#include "diag_gate.h"

namespace DebugPipe {
WWMI_DEBUG_FN void SetListening(bool on) WWMI_DEBUG_BODY();
WWMI_DEBUG_FN void RunQueued() WWMI_DEBUG_BODY(); // present thread
WWMI_DEBUG_FN void Shutdown() WWMI_DEBUG_BODY(); // add-on unload: no thread outlives the DLL
}
